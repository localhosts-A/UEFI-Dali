// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include <Library/Mt6991UfsDmaLib.h>

STATIC BOOLEAN Span (UINT64 Start, UINTN Bytes, UINT64 Limit) {
  return Bytes != 0 && Start <= Limit && (UINT64)(Bytes - 1) <= Limit - Start;
}

// UC may be mapped as Device memory by the platform. Byte accesses avoid
// unaligned/vector/cache-zero instructions in an optimized generic memcpy.
STATIC VOID UcZero (VOID *Cpu, UINTN Bytes) {
  volatile UINT8 *P = Cpu;
  for (UINTN I = 0; I < Bytes; I++) { P[I] = 0; }
}

STATIC VOID UcCopyIn (VOID *Uc, CONST VOID *Host, UINTN Bytes) {
  volatile UINT8 *To = Uc; CONST UINT8 *From = Host;
  for (UINTN I = 0; I < Bytes; I++) { To[I] = From[I]; }
}

STATIC VOID UcCopyOut (VOID *Host, CONST VOID *Uc, UINTN Bytes) {
  UINT8 *To = Host; CONST volatile UINT8 *From = Uc;
  for (UINTN I = 0; I < Bytes; I++) { To[I] = From[I]; }
}

STATIC EFI_STATUS FreeRegion (MT6991_UFS_DMA *D, MT6991_DMA_REGION *R) {
  EFI_STATUS Status = D->Ops.Free (R, D->Ops.Context);
  if (EFI_ERROR (Status)) { R->Quarantined = TRUE; D->Poisoned = TRUE; }
  else { ZeroMem (R, sizeof *R); }
  return Status;
}

STATIC EFI_STATUS NewRegion (MT6991_UFS_DMA *D, UINTN Pages, BOOLEAN Common, UINTN *Index) {
  if (!Pages || Pages > MT6991_UFS_DMA_MAX_TRANSFER / EFI_PAGE_SIZE) { return EFI_BAD_BUFFER_SIZE; }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    MT6991_DMA_REGION *R = &D->Regions[I];
    if (R->Live) { continue; }
    ZeroMem (R, sizeof *R);
    EFI_STATUS Status = D->Ops.Allocate (R, Pages, D->Limit, D->Ops.Context);
    if (!EFI_ERROR (Status) && (!R->Live || R->Cpu == NULL || R->Pages != Pages ||
        ((UINTN)R->Cpu & (EFI_PAGE_SIZE - 1)) || (R->Device & (EFI_PAGE_SIZE - 1)) ||
        !Span ((UINTN)R->Cpu, Pages * EFI_PAGE_SIZE, MAX_UINTN) ||
        !Span (R->Device, Pages * EFI_PAGE_SIZE, D->Limit))) {
      Status = EFI_DEVICE_ERROR;
    }
    if (EFI_ERROR (Status)) {
      if (R->Live) { FreeRegion (D, R); }
      return Status;
    }
    R->Common = Common; *Index = I;
    UcZero (R->Cpu, Pages * EFI_PAGE_SIZE);
    D->Ops.Fence (D->Ops.Context);
    return EFI_SUCCESS;
  }
  return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS Mt6991UfsDmaInit (MT6991_UFS_DMA *D, CONST MT6991_DMA_OPS *Ops, UINT64 Limit) {
  if (D == NULL || Ops == NULL || Ops->Allocate == NULL || Ops->Free == NULL || Ops->Fence == NULL ||
      Limit < EFI_PAGE_SIZE - 1 || Limit > 0xFFFFFFFFULL) { return EFI_INVALID_PARAMETER; }
  if (D->Initialized) { return EFI_ALREADY_STARTED; }
  ZeroMem (D, sizeof *D); D->Ops = *Ops; D->Limit = Limit; D->Initialized = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Available (MT6991_UFS_DMA *D) {
  if (D == NULL || !D->Initialized) { return EFI_INVALID_PARAMETER; }
  if (D->Poisoned || D->Stopped) { return EFI_DEVICE_ERROR; }
  return D->Busy ? EFI_NOT_READY : EFI_SUCCESS;
}

EFI_STATUS Mt6991UfsDmaAllocate (MT6991_UFS_DMA *D, UINTN Pages, VOID **Host) {
  EFI_STATUS Status = Available (D); UINTN Index;
  if (Host == NULL) { return EFI_INVALID_PARAMETER; }
  *Host = NULL;
  if (EFI_ERROR (Status)) { return Status; }
  Status = NewRegion (D, Pages, TRUE, &Index);
  if (!EFI_ERROR (Status)) { *Host = D->Regions[Index].Cpu; }
  return Status;
}

EFI_STATUS Mt6991UfsDmaFree (MT6991_UFS_DMA *D, UINTN Pages, VOID *Host) {
  if (D == NULL || !D->Initialized || Host == NULL) { return EFI_INVALID_PARAMETER; }
  if (D->Busy || (D->Poisoned && !D->Stopped)) { return EFI_NOT_READY; }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    MT6991_DMA_REGION *R = &D->Regions[I];
    if (R->Live && R->Common && R->Cpu == Host && R->Pages == Pages) {
      if (R->References) { return EFI_ACCESS_DENIED; }
      return FreeRegion (D, R);
    }
  }
  return EFI_INVALID_PARAMETER;
}

EFI_STATUS Mt6991UfsDmaMap (MT6991_UFS_DMA *D, MT6991_DMA_OPERATION Operation, VOID *Host,
                           UINTN Bytes, UINT64 *Device, UINT64 *Id) {
  EFI_STATUS Status = Available (D); UINTN Region = MT6991_UFS_DMA_SLOTS, Offset = 0;
  if (Device == NULL || Id == NULL) { return EFI_INVALID_PARAMETER; }
  *Device = 0; *Id = 0;
  if (EFI_ERROR (Status)) { return Status; }
  if ((UINT32)Operation > Mt6991DmaCommon || Host == NULL || Bytes > MT6991_UFS_DMA_MAX_TRANSFER ||
      !Span ((UINTN)Host, Bytes, MAX_UINTN)) { return EFI_INVALID_PARAMETER; }
  MT6991_DMA_MAP *M = NULL;
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) { if (!D->Maps[I].Live) { M = &D->Maps[I]; break; } }
  if (M == NULL || D->NextId == MAX_UINT64) { return EFI_OUT_OF_RESOURCES; }
  if (Operation == Mt6991DmaCommon) {
    for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
      MT6991_DMA_REGION *R = &D->Regions[I];
      UINTN Start = (UINTN)R->Cpu, Address = (UINTN)Host, Size = R->Pages * EFI_PAGE_SIZE;
      if (R->Live && R->Common && !R->Quarantined && Address >= Start && Bytes <= Size &&
          Address - Start <= Size - Bytes) {
        if (R->References) { return EFI_ACCESS_DENIED; }
        Region = I; Offset = Address - Start; break;
      }
    }
    if (Region == MT6991_UFS_DMA_SLOTS) { return EFI_UNSUPPORTED; }
  } else {
    Status = NewRegion (D, EFI_SIZE_TO_PAGES (Bytes), FALSE, &Region);
    if (EFI_ERROR (Status)) { return Status; }
    if (Operation == Mt6991DmaDeviceRead) { UcCopyIn (D->Regions[Region].Cpu, Host, Bytes); }
  }
  ZeroMem (M, sizeof *M); M->Live = TRUE; M->Id = ++D->NextId;
  M->Region = Region; M->Offset = Offset; M->Length = Bytes; M->Host = Host; M->Operation = Operation;
  ++D->Regions[Region].References;
  D->Ops.Fence (D->Ops.Context);
  *Device = D->Regions[Region].Device + Offset; *Id = M->Id;
  return EFI_SUCCESS;
}

EFI_STATUS Mt6991UfsDmaUnmap (MT6991_UFS_DMA *D, UINT64 Id) {
  if (D == NULL || !D->Initialized || Id == 0) { return EFI_INVALID_PARAMETER; }
  if (D->Busy || (D->Poisoned && !D->Stopped)) { return EFI_NOT_READY; }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    MT6991_DMA_MAP *M = &D->Maps[I];
    if (!M->Live || M->Id != Id) { continue; }
    MT6991_DMA_REGION *R = &D->Regions[M->Region];
    D->Ops.Fence (D->Ops.Context);
    if (M->Operation == Mt6991DmaDeviceWrite && M->CopyBack && !D->Poisoned) {
      UcCopyOut (M->Host, (UINT8 *)R->Cpu + M->Offset, M->Length); M->CopyBack = FALSE;
    }
    if (!R->Common) {
      EFI_STATUS Status = FreeRegion (D, R);
      if (EFI_ERROR (Status)) { return Status; }
    } else { --R->References; }
    ZeroMem (M, sizeof *M); return EFI_SUCCESS;
  }
  return EFI_INVALID_PARAMETER;
}

EFI_STATUS Mt6991UfsDmaResolve (MT6991_UFS_DMA *D, UINT64 Device, UINTN Bytes,
                               BOOLEAN RequireCommon, VOID **Cpu) {
  if (D == NULL || !D->Initialized || Cpu == NULL || !Span (Device, Bytes, D->Limit)) { return EFI_INVALID_PARAMETER; }
  *Cpu = NULL;
  if (D->Poisoned || D->Stopped) { return EFI_DEVICE_ERROR; }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    MT6991_DMA_MAP *M = &D->Maps[I]; MT6991_DMA_REGION *R = &D->Regions[M->Region];
    UINT64 Base = R->Device + M->Offset;
    if (M->Live && R->Live && !R->Quarantined && (!RequireCommon || R->Common) &&
        Device >= Base && Bytes <= M->Length && Device - Base <= M->Length - Bytes) {
      *Cpu = (UINT8 *)R->Cpu + M->Offset + (UINTN)(Device - Base); return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS Mt6991UfsDmaSubmit (MT6991_UFS_DMA *D) {
  EFI_STATUS Status = Available (D);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    if (D->Maps[I].Live && D->Maps[I].Operation != Mt6991DmaCommon && D->Maps[I].Submitted) {
      return EFI_NOT_READY;
    }
  }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    if (D->Maps[I].Live) { D->Maps[I].Submitted = TRUE; D->Maps[I].CopyBack = FALSE; }
  }
  D->Ops.Fence (D->Ops.Context); D->Busy = TRUE; return EFI_SUCCESS;
}

EFI_STATUS Mt6991UfsDmaComplete (MT6991_UFS_DMA *D, BOOLEAN DataValid) {
  if (D == NULL || !D->Initialized || !D->Busy || D->Poisoned) { return EFI_INVALID_PARAMETER; }
  D->Ops.Fence (D->Ops.Context); D->Busy = FALSE;
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    D->Maps[I].CopyBack = D->Maps[I].Live && D->Maps[I].Submitted && DataValid;
  }
  return EFI_SUCCESS;
}

VOID Mt6991UfsDmaPoison (MT6991_UFS_DMA *D) { if (D != NULL) { D->Poisoned = TRUE; } }

EFI_STATUS Mt6991UfsDmaStopped (MT6991_UFS_DMA *D, UINT32 Hce) {
  if (D == NULL || !D->Initialized) { return EFI_INVALID_PARAMETER; }
  D->Poisoned = TRUE;
  if (Hce != 0) { D->Stopped = FALSE; return EFI_NOT_READY; }
  D->Ops.Fence (D->Ops.Context); D->Busy = FALSE; D->Stopped = TRUE;
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) { D->Maps[I].CopyBack = FALSE; }
  return EFI_SUCCESS;
}

EFI_STATUS Mt6991UfsDmaReleaseAll (MT6991_UFS_DMA *D) {
  if (D == NULL || !D->Initialized) { return EFI_INVALID_PARAMETER; }
  if (!D->Stopped || D->Busy) { return EFI_NOT_READY; }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    if (D->Maps[I].Live) {
      EFI_STATUS Status = Mt6991UfsDmaUnmap (D, D->Maps[I].Id);
      if (EFI_ERROR (Status)) { return Status; }
    }
  }
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    if (D->Regions[I].Live) {
      EFI_STATUS Status = FreeRegion (D, &D->Regions[I]);
      if (EFI_ERROR (Status)) { return Status; }
    }
  }
  return EFI_SUCCESS;
}
