// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include <Library/Mt6991UfsPolicy.h>

STATIC UINT16 Be16 (CONST UINT8 *P) { return (UINT16)(((UINT16)P[0] << 8) | P[1]); }
STATIC UINT32 Be32 (CONST UINT8 *P) { return ((UINT32)Be16 (P) << 16) | Be16 (P + 2); }
STATIC UINT64 Be64 (CONST UINT8 *P) { return ((UINT64)Be32 (P) << 32) | Be32 (P + 4); }
STATIC UINT16 Le16 (CONST UINT8 *P) { return (UINT16)(((UINT16)P[1] << 8) | P[0]); }
STATIC UINT32 Le32 (CONST UINT8 *P) { return ((UINT32)Le16 (P + 2) << 16) | Le16 (P); }

EFI_STATUS Mt6991UfsReadOnlyCommand (CONST MT6991_UFS_GEOMETRY *G,
  UINT8 Lun, CONST UINT8 *Cdb, UINTN CdbBytes, UINT32 Bytes) {
  UINTN Length;
  UINT64 Lba = 0, Count = 0;
  if (Lun >= 8 || Cdb == NULL || CdbBytes < 6 || Bytes > MT6991_UFS_DMA_MAX_TRANSFER) { return EFI_ACCESS_DENIED; }
  switch (Cdb[0]) {
    case 0x00: Length = 6; if (Bytes) { return EFI_INVALID_PARAMETER; } break;
    case 0x03: case 0x12: case 0x1A: Length = 6; break;
    case 0x5A: Length = 10; break;
    case 0x25: Length = 10; if (Bytes != 8) { return EFI_INVALID_PARAMETER; } break;
    case 0x9E:
      Length = 16;
      if (CdbBytes != Length || Cdb[1] != 0x10 || Bytes < 32 || Be32 (Cdb + 10) != Bytes) { return EFI_INVALID_PARAMETER; }
      break;
    case 0xA0: Length = 12; break;
    case 0x35: case 0x91:
      Length = Cdb[0] == 0x35 ? 10 : 16;
      if (CdbBytes != Length || Bytes || (Cdb[1] & 2)) { return EFI_INVALID_PARAMETER; }
      break;
    case 0x28: case 0x88:
      Length = Cdb[0] == 0x28 ? 10 : 16;
      if (CdbBytes != Length || G == NULL || !G[Lun].Valid) { return EFI_NOT_READY; }
      Lba = Length == 10 ? Be32 (Cdb + 2) : Be64 (Cdb + 2);
      Count = Length == 10 ? Be16 (Cdb + 7) : Be32 (Cdb + 10);
      if (!G[Lun].BlockSize || Lba > G[Lun].Blocks || Count > G[Lun].Blocks - Lba ||
          Count > MT6991_UFS_DMA_MAX_TRANSFER / G[Lun].BlockSize ||
          Bytes != Count * G[Lun].BlockSize) { return EFI_INVALID_PARAMETER; }
      break;
    default: return EFI_WRITE_PROTECTED;
  }
  return CdbBytes == Length ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

EFI_STATUS Mt6991UfsReadOnlyQuery (UINT8 Op, UINT8 Id, UINT8 Index,
  UINT8 Selector, UINT32 Bytes, BOOLEAN InitAllowed) {
  if (Selector != 0) { return EFI_ACCESS_DENIED; }
  if (Op == 1) { return Bytes && Bytes <= 65535 ? EFI_SUCCESS : EFI_INVALID_PARAMETER; }
  if (Op == 3 || Op == 5) { return Bytes == 0 ? EFI_SUCCESS : EFI_INVALID_PARAMETER; }
  // fDeviceInit is the sole permitted control write; never provisioning/WP/RPMB.
  if (Op == 6 && Id == 1 && Index == 0 && Bytes == 0 && InitAllowed) { return EFI_SUCCESS; }
  return EFI_WRITE_PROTECTED;
}

STATIC EFI_STATUS DataMapping (MT6991_UFS_DMA *D, UINT32 Expected, MT6991_DMA_MAP **Result) {
  *Result = NULL;
  for (UINTN I = 0; I < MT6991_UFS_DMA_SLOTS; I++) {
    MT6991_DMA_MAP *M = &D->Maps[I];
    if (!M->Live || M->Operation == Mt6991DmaCommon) { continue; }
    if (*Result != NULL || M->Operation != Mt6991DmaDeviceWrite ||
        M->Length != ((Expected + 3U) & ~3U)) { return EFI_ACCESS_DENIED; }
    *Result = M;
  }
  return ((*Result != NULL) == (Expected != 0)) ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

EFI_STATUS Mt6991UfsValidateTransfer (MT6991_UFS_DMA *D, UINT64 Trl,
  CONST MT6991_UFS_GEOMETRY *G, BOOLEAN InitAllowed, MT6991_UFS_PENDING *P) {
  VOID *Cpu;
  EFI_STATUS Status;
  UINT8 *T, *Q;
  UINTN RuOffset, RuBytes, PrdtOffset, PrdtCount, Size;
  UINT64 CommandAddress;
  MT6991_DMA_MAP *Map;
  if (P == NULL || (Trl & 1023)) { return EFI_INVALID_PARAMETER; }
  ZeroMem (P, sizeof *P);
  Status = Mt6991UfsDmaResolve (D, Trl, 32, TRUE, &Cpu);
  if (EFI_ERROR (Status)) { return Status; }
  T = Cpu;
  UINT32 Header = Le32 (T);
  UINT32 Direction = (Header >> 25) & 3;
  if ((Header & 0xF8FFFFFFU) != 0x10000000U || Le32 (T + 4) || Le32 (T + 8) != 15 || Le32 (T + 12) ||
      (Le32 (T + 16) & 127) || Le32 (T + 20)) { return EFI_ACCESS_DENIED; }
  CommandAddress = Le32 (T + 16);
  RuOffset = Le16 (T + 26) * 4U; RuBytes = Le16 (T + 24) * 4U;
  PrdtOffset = Le16 (T + 30) * 4U; PrdtCount = Le16 (T + 28);
  if (RuOffset != 32 || RuBytes < 32 || PrdtCount > 4 || (RuBytes & 7)) { return EFI_ACCESS_DENIED; }
  Size = RuOffset + RuBytes;
  if (PrdtCount) {
    if (PrdtOffset != Size) { return EFI_ACCESS_DENIED; }
    Size += PrdtCount * 16;
  }
  Status = Mt6991UfsDmaResolve (D, CommandAddress, Size, TRUE, &Cpu);
  if (EFI_ERROR (Status)) { return Status; }
  Q = Cpu;
  if (CommandAddress <= Trl ? Trl - CommandAddress < Size : CommandAddress - Trl < 32) { return EFI_ACCESS_DENIED; }
  if (Q[0] != 0 && Q[0] != 1 && Q[0] != 0x16) { return EFI_ACCESS_DENIED; }
  if (Q[8] || Be16 (Q + 10)) { return EFI_ACCESS_DENIED; }
  P->Trd = T; P->Request = Q; P->Response = Q + RuOffset; P->ResponseBytes = RuBytes;
  P->Transaction = Q[0]; P->Lun = Q[2]; P->Tag = Q[3];
  if (Q[0] == 1) {
    P->Expected = Be32 (Q + 12);
    UINT8 Op = Q[16];
    UINTN CdbBytes = (Op == 0 || Op == 3 || Op == 0x12 || Op == 0x1A) ? 6 :
                    (Op == 0xA0 ? 12 : (Op == 0x88 || Op == 0x91 || Op == 0x9E ? 16 : 10));
    Status = Mt6991UfsReadOnlyCommand (G, Q[2], Q + 16, CdbBytes, P->Expected);
    if (EFI_ERROR (Status)) { return Status; }
    if (RuBytes != 56 || Direction != (P->Expected ? 2U : 0U) || Q[1] != (P->Expected ? 0x41 : 1) || Q[4]) { return EFI_ACCESS_DENIED; }
  } else if (Q[0] == 0x16) {
    UINT8 Op = Q[12]; UINT32 Bytes = Be16 (Q + 18);
    Status = Mt6991UfsReadOnlyQuery (Op, Q[13], Q[14], Q[15], Bytes, InitAllowed);
    if (EFI_ERROR (Status)) { return Status; }
    if (Q[5] != (Op == 6 ? 0x81 : 1) || Direction != (Op == 6 ? 1U : 2U) ||
        PrdtCount || RuBytes != 32U + ((Bytes + 7U) & ~7U) || Be32 (Q + 20)) { return EFI_ACCESS_DENIED; }
  } else if (RuBytes != 32 || Direction || PrdtCount) { return EFI_ACCESS_DENIED; }
  Status = DataMapping (D, P->Expected, &Map);
  if (EFI_ERROR (Status)) { return Status; }
  if (Map != NULL) {
    UINT64 Base = D->Regions[Map->Region].Device + Map->Offset;
    UINTN Remaining = Map->Length, Offset = 0;
    for (UINTN I = 0; I < PrdtCount; I++) {
      UINT8 *Prd = Q + PrdtOffset + I * 16;
      UINT32 Count = Le32 (Prd + 12) + 1;
      if (Le32 (Prd) != Base + Offset || Le32 (Prd + 4) || Le32 (Prd + 8) ||
          (Le32 (Prd) & 3) || Count > 0x40000 || Count > Remaining || (Count & 3)) { return EFI_ACCESS_DENIED; }
      Remaining -= Count; Offset += Count;
    }
    if (Remaining) { return EFI_ACCESS_DENIED; }
    Status = Mt6991UfsDmaResolve (D, Base, Map->Length, FALSE, &Cpu);
    if (EFI_ERROR (Status)) { return Status; }
    P->Data = Cpu;
  } else if (PrdtCount) { return EFI_ACCESS_DENIED; }
  return EFI_SUCCESS;
}

EFI_STATUS Mt6991UfsValidateResponse (MT6991_UFS_PENDING *P, MT6991_UFS_GEOMETRY *G, BOOLEAN *Valid) {
  UINT8 *R = P->Response, *Q = P->Request;
  *Valid = FALSE;
  if (Le32 (P->Trd + 8) != 0 || R[0] != (P->Transaction | 0x20) ||
      R[3] != P->Tag || R[8]) { return EFI_DEVICE_ERROR; }
  if (P->Transaction == 1) {
    UINT32 Residual = Be32 (R + 12);
    UINT16 Sense = Be16 (R + 32), Segment = Be16 (R + 10);
    if (R[2] != P->Lun || (R[1] & 0x40) || Residual > P->Expected ||
        (Residual && !(R[1] & 0x20)) || Sense > 18 || Segment > 20 ||
        (Sense && Segment < Sense + 2U)) { return EFI_DEVICE_ERROR; }
    *Valid = R[6] == 0 && R[7] == 0;
    if (*Valid && P->Data != NULL && !Residual && (Q[16] == 0x25 || Q[16] == 0x9E)) {
      UINT64 Last = Q[16] == 0x25 ? Be32 (P->Data) : Be64 (P->Data);
      UINT32 Block = Be32 (P->Data + (Q[16] == 0x25 ? 4 : 8));
      if (Last == MAX_UINT64 || (Q[16] == 0x25 && Last == 0xFFFFFFFFU)) { return EFI_SUCCESS; }
      if (Block < 512 || Block > 65536 || (Block & (Block - 1))) { return EFI_DEVICE_ERROR; }
      G[P->Lun].Blocks = Last + 1; G[P->Lun].BlockSize = Block; G[P->Lun].Valid = TRUE;
    }
  } else if (P->Transaction == 0x16) {
    UINT16 Bytes = Be16 (R + 18);
    if (R[5] != Q[5] || CompareMem (R + 12, Q + 12, 4) ||
        (Q[12] == 1 && (Bytes > Be16 (Q + 18) || Be16 (R + 10) != Bytes)) ||
        (Q[12] != 1 && Be16 (R + 10))) { return EFI_DEVICE_ERROR; }
  } else if (R[6] || Be16 (R + 10)) { return EFI_DEVICE_ERROR; }
  return EFI_SUCCESS;
}
