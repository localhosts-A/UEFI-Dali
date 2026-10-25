// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include "UfsShutdown.h"

#define HCI 0x16810000U
#define DEADLINE_US 3000000U

STATIC EFI_STATUS Failure (MT6991_UFS_DMA *D, DALI_UFS_SHUTDOWN *S, EFI_STATUS Status) {
  S->Status = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  if (D->Initialized) { Mt6991UfsDmaPoison (D); }
  return S->Status;
}

STATIC BOOLEAN Write (CONST MT6991_UFS_IO *Io, UINT32 Offset, UINT32 Value) {
  if (!Io->Write32 (HCI + Offset, Value, Io->Context)) { return FALSE; }
  Io->Fence (Io->Context); return TRUE;
}

STATIC EFI_STATUS Wait (CONST MT6991_UFS_IO *Io, UINT32 Offset, UINT32 Expected, UINT32 Deadline) {
  for (UINT32 Elapsed = 0; ; Elapsed += 10) {
    UINT32 Value = Io->Read32 (HCI + Offset, Io->Context);
    if (Value == 0xFFFFFFFFU) { return EFI_DEVICE_ERROR; }
    if (Value == Expected) { return EFI_SUCCESS; }
    if (Elapsed >= Deadline) { return EFI_TIMEOUT; }
    Io->Stall (10, Io->Context);
  }
}

STATIC EFI_STATUS Finish (MT6991_UFS_DMA *D, MT6991_UFS_PENDING *P,
  MT6991_UFS_GEOMETRY *G, volatile UINT64 *Counts, CONST MT6991_UFS_IO *Io) {
  BOOLEAN Valid;
  if (P->Trd == NULL || P->Request == NULL || P->Response == NULL || P->Lun >= 8 ||
      P->ResponseBytes < (P->Transaction == 1 ? 56U : 32U)) { return EFI_DEVICE_ERROR; }
  Io->Fence (Io->Context);
  UINT32 Is = Io->Read32 (HCI + 0x20, Io->Context);
  if (Is == 0xFFFFFFFFU || (Is & 0x70884U)) { return EFI_DEVICE_ERROR; }
  EFI_STATUS Status = Mt6991UfsValidateResponse (P, G, &Valid);
  if (Status != EFI_SUCCESS) { return Status; }
  Status = Mt6991UfsDmaComplete (D, Valid);
  if (Status != EFI_SUCCESS) { return Status; }
  if ((P->Transaction == 1 && !Valid) || (P->Transaction == 0x16 && P->Response[6])) { return EFI_DEVICE_ERROR; }
  if (Valid && P->Transaction == 1 && (P->Opcode == 0x35 || P->Opcode == 0x91)) {
    if (Counts[P->Lun] == MAX_UINT64) { return EFI_DEVICE_ERROR; }
    ++Counts[P->Lun];
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Drain (MT6991_UFS_DMA *D, MT6991_UFS_PENDING *P,
  MT6991_UFS_GEOMETRY *G, volatile UINT64 *Counts, CONST MT6991_UFS_IO *Io) {
  for (UINT32 Elapsed = 0; ; Elapsed += 10) {
    if (Io->Read32 (HCI + 0x34, Io->Context) != 1 || Io->Read32 (HCI + 0x78, Io->Context) != 0) { return EFI_DEVICE_ERROR; }
    UINT32 Doorbell = Io->Read32 (HCI + 0x58, Io->Context);
    UINT32 Is = Io->Read32 (HCI + 0x20, Io->Context);
    if ((Doorbell & ~1U) || Is == 0xFFFFFFFFU || (Is & 0x70884U) || (Doorbell && !D->Busy)) { return EFI_DEVICE_ERROR; }
    if (!Doorbell) { return D->Busy ? Finish (D, P, G, Counts, Io) : EFI_SUCCESS; }
    if (Elapsed >= DEADLINE_US) { return EFI_TIMEOUT; }
    Io->Stall (10, Io->Context);
  }
}

STATIC VOID Le16 (UINT8 *P, UINT16 V) { P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8); }
STATIC VOID Le32 (UINT8 *P, UINT32 V) { Le16 (P, (UINT16)V); Le16 (P + 2, (UINT16)(V >> 16)); }

EFI_STATUS DaliUfsDrainAndSync (MT6991_UFS_DMA *D, MT6991_UFS_PENDING *P, MT6991_UFS_GEOMETRY *G,
  volatile UINT64 *Counts, CONST MT6991_UFS_IO *Io, DALI_UFS_SHUTDOWN *S) {
  if (D == NULL || S == NULL) { return EFI_INVALID_PARAMETER; }
  if (S->Attempted) { return S->Status; }
  S->Attempted = TRUE; S->Status = EFI_NOT_READY;
  if (!D->Initialized) { S->Completed = TRUE; S->Status = EFI_SUCCESS; return S->Status; }
  if (P == NULL || G == NULL || Counts == NULL || Io == NULL ||
      Io->Read32 == NULL || Io->Write32 == NULL || Io->Stall == NULL || Io->Fence == NULL) { return Failure (D, S, EFI_INVALID_PARAMETER); }
  if (D->Poisoned || D->Stopped) { return Failure (D, S, EFI_DEVICE_ERROR); }
  EFI_STATUS Status = Drain (D, P, G, Counts, Io);
  if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
  // Retire completed (or never submitted) data mappings before a no-data CDB.
  // Copyback occurs only for the already validated completion above.
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    if (D->Maps[I].Live && D->Maps[I].Operation != Mt6991DmaCommon) {
      Status = Mt6991UfsDmaUnmap (D, D->Maps[I].Id);
      if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
    }
  }
  // Original queues are idle and no public caller can submit. Stop them before
  // replacing the transfer-list base; retain every old common mapping for Stop.
  if (!Write (Io, 0x60, 0) || !Write (Io, 0x80, 0)) { return Failure (D, S, EFI_DEVICE_ERROR); }
  Status = Wait (Io, 0x60, 0, 100000);
  if (Status == EFI_SUCCESS) { Status = Wait (Io, 0x80, 0, 100000); }
  if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
  if (Io->Read32 (HCI + 0x58, Io->Context) || Io->Read32 (HCI + 0x78, Io->Context)) { return Failure (D, S, EFI_DEVICE_ERROR); }
  UINT8 Needed = 0;
  for (UINTN Lun = 0; Lun < 8; Lun++) {
    if (!G[Lun].Valid) { continue; }
    if (!G[Lun].Blocks || G[Lun].BlockSize < 512 || G[Lun].BlockSize > 65536 ||
        (G[Lun].BlockSize & (G[Lun].BlockSize - 1))) { return Failure (D, S, EFI_DEVICE_ERROR); }
    Needed |= (UINT8)(1U << Lun);
  }
  if (Needed != 0) {
    VOID *Host; UINT64 Device, Mapping;
    Status = Mt6991UfsDmaAllocate (D, 1, &Host);
    if (Status == EFI_SUCCESS) { Status = Mt6991UfsDmaMap (D, Mt6991DmaCommon, Host, EFI_PAGE_SIZE, &Device, &Mapping); }
    if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
    // One page, two nonoverlapping 128/1024-aligned regions, both DMA-owned.
    UINT8 *Trd = Host, *Ucd = (UINT8 *)Host + 1024;
    UINT8 Tag = (UINT8)(P->Tag + 1);
    if (!Write (Io, 0x50, (UINT32)Device) || !Write (Io, 0x54, 0) || !Write (Io, 0x60, 1)) { return Failure (D, S, EFI_DEVICE_ERROR); }
    Status = Wait (Io, 0x60, 1, 100000);
    if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
    for (UINT8 Lun = 0; Lun < 8; Lun++) {
      if (!(Needed & (1U << Lun))) { continue; }
      if (Counts[Lun] == MAX_UINT64) { return Failure (D, S, EFI_DEVICE_ERROR); }
      ZeroMem (Trd, 32); ZeroMem (Ucd, 88);
      Le32 (Trd, 0x11000000); Le32 (Trd + 8, 15); Le32 (Trd + 16, (UINT32)(Device + 1024));
      Le16 (Trd + 24, 14); Le16 (Trd + 26, 8); Le16 (Trd + 30, 22);
      Ucd[0] = 1; Ucd[1] = 1; Ucd[2] = Lun; Ucd[3] = Tag++; Ucd[16] = 0x35;
      Status = Mt6991UfsValidateTransfer (D, Device, G, NULL, FALSE, P);
      if (Status == EFI_SUCCESS) { Status = Mt6991UfsDmaSubmit (D); }
      if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
      ++S->Commands;
      if (!Write (Io, 0x20, 1) || !Write (Io, 0x58, 1)) { return Failure (D, S, EFI_DEVICE_ERROR); }
      Status = Drain (D, P, G, Counts, Io);
      if (Status != EFI_SUCCESS) { return Failure (D, S, Status); }
      S->SyncedLuns |= (UINT8)(1U << Lun);
    }
  }
  S->Completed = TRUE; S->Status = EFI_SUCCESS; return S->Status;
}
