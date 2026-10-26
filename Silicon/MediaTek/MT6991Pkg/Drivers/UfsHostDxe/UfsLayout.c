// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include "UfsLayout.h"

typedef struct {
  UINT64 First, Last, EntriesLba;
  UINT32 Count, Stride, HeaderCrc, EntriesCrc;
  UINT8 DiskGuid[16];
} GPT_HEADER;

STATIC UINT32 Le32 (CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
STATIC UINT64 Le64 (CONST UINT8 *P) { return Le32 (P) | ((UINT64)Le32 (P + 4) << 32); }
STATIC BOOLEAN Nonzero (CONST UINT8 *P, UINTN Bytes) {
  UINT8 Any = 0;
  for (UINTN I = 0; I < Bytes; ++I) { Any |= P[I]; }
  return Any != 0;
}
STATIC UINT32 Crc32 (CONST UINT8 *P, UINTN Bytes, BOOLEAN Header) {
  UINT32 Crc = MAX_UINT32;
  for (UINTN I = 0; I < Bytes; ++I) {
    Crc ^= Header && I >= 16 && I < 20 ? 0 : P[I];
    for (UINTN Bit = 0; Bit < 8; ++Bit) { Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xEDB88320U : 0); }
  }
  return ~Crc;
}
STATIC BOOLEAN SameGeometry (CONST MT6991_UFS_GEOMETRY *A, CONST MT6991_UFS_GEOMETRY *B) {
  return A->Valid == B->Valid && A->Blocks == B->Blocks && A->BlockSize == B->BlockSize &&
    A->WriteProtectionKnown == B->WriteProtectionKnown && A->WriteProtected == B->WriteProtected;
}
STATIC EFI_STATUS ReadOnce (DALI_UFS_LAYOUT_READ Read, VOID *Context, UINT8 Lun,
  UINT64 Lba, UINTN Bytes, VOID *Buffer, CONST MT6991_UFS_GEOMETRY *Live,
  CONST MT6991_UFS_GEOMETRY *Snapshot) {
  if (!SameGeometry (Live, Snapshot)) { return EFI_MEDIA_CHANGED; }
  EFI_STATUS Status = Read (Context, Lun, Lba, Bytes, Buffer);
  if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
  return SameGeometry (Live, Snapshot) ? EFI_SUCCESS : EFI_MEDIA_CHANGED;
}
STATIC BOOLEAN Header (CONST UINT8 *P, CONST MT6991_UFS_GEOMETRY *G,
  BOOLEAN Primary, GPT_HEADER *H) {
  UINT64 Last = G->Blocks - 1;
  UINT32 Bytes = Le32 (P + 12);
  if (CompareMem (P, "EFI PART", 8) || Le32 (P + 8) != 0x10000 ||
      Bytes < 92 || Bytes > G->BlockSize || Le32 (P + 20) ||
      Le32 (P + 16) != Crc32 (P, Bytes, TRUE) ||
      Le64 (P + 24) != (Primary ? 1 : Last) || Le64 (P + 32) != (Primary ? Last : 1)) { return FALSE; }
  H->First = Le64 (P + 40); H->Last = Le64 (P + 48); H->EntriesLba = Le64 (P + 72);
  H->Count = Le32 (P + 80); H->Stride = Le32 (P + 84);
  H->HeaderCrc = Le32 (P + 16); H->EntriesCrc = Le32 (P + 88);
  CopyMem (H->DiskGuid, P + 56, sizeof H->DiskGuid);
  if (H->First <= 1 || H->First > H->Last || H->Last >= Last ||
      H->Count == 0 || H->Count > DALI_UFS_GPT_MAX_ENTRIES || H->Stride < 128 ||
      H->Stride > 4096 || (H->Stride & (H->Stride - 1)) ||
      (UINT64)H->Count * H->Stride > DALI_UFS_GPT_MAX_BYTES) { return FALSE; }
  UINT64 Blocks = ((UINT64)H->Count * H->Stride + G->BlockSize - 1) / G->BlockSize;
  if (H->EntriesLba < 2 || H->EntriesLba >= Last || Blocks > Last - H->EntriesLba) { return FALSE; }
  // Both arrays must be outside the usable area and outside both headers.
  return Primary ? H->EntriesLba + Blocks <= H->First : H->EntriesLba > H->Last;
}
STATIC BOOLEAN TargetName (CONST UINT8 *P, UINT32 Target) {
  STATIC CONST UINT8 BootB[14] = { 'b',0,'o',0,'o',0,'t',0,'_',0,'b',0,0,0 };
  STATIC CONST UINT8 Expdb[12] = { 'e',0,'x',0,'p',0,'d',0,'b',0,0,0 };
  STATIC CONST UINT8 UserdataLogs[24] = {
    'u',0,'n',0,'i',0,'x',0,'n',0,'u',0,'_',0,'l',0,'o',0,'g',0,'s',0,0,0
  };
  UINTN Bytes = Target == MT6991_UFS_TARGET_EXPDB ? sizeof Expdb :
                Target == MT6991_UFS_TARGET_USERDATA_LOGS ? sizeof UserdataLogs : sizeof BootB;
  CONST UINT8 *Name = Target == MT6991_UFS_TARGET_EXPDB ? Expdb :
                      Target == MT6991_UFS_TARGET_USERDATA_LOGS ? UserdataLogs : BootB;
  return CompareMem (P + 56, Name, Bytes) == 0;
}
STATIC VOID Guid (CONST UINT8 *Little, UINT8 *Canonical) {
  STATIC CONST UINT8 Order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
  for (UINTN I = 0; I < 16; ++I) { Canonical[I] = Little[Order[I]]; }
}

EFI_STATUS DaliUfsInspectScratchLayout (CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST MT6991_UFS_SCRATCH_GRANT *Expected, DALI_UFS_LAYOUT_READ Read, VOID *Context,
  VOID *Workspace, UINTN WorkspaceBytes, DALI_UFS_LAYOUT *Observed) {
  if (Observed == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Observed, sizeof *Observed);
  if (Geometry == NULL || Expected == NULL || Read == NULL || Workspace == NULL || Expected->Lun >= 8) {
    return EFI_INVALID_PARAMETER;
  }
  if (WorkspaceBytes < DALI_UFS_LAYOUT_WORK_BYTES) { return EFI_BUFFER_TOO_SMALL; }
  UINT64 Absolute, Blocks;
  EFI_STATUS Status = Mt6991UfsScratchMapRange (Geometry, Expected, Expected->Lun, 0,
    Expected->BlockSize, TRUE, &Absolute, &Blocks);
  if (Status != EFI_SUCCESS) { return Status; }
  MT6991_UFS_GEOMETRY G = Geometry[Expected->Lun];
  if (G.Blocks < 4) { return EFI_VOLUME_CORRUPTED; }
  // Preserve every supported IoAlign at each sub-buffer, even when the media
  // block size is smaller than its I/O alignment.
  UINT8 *Primary = Workspace, *Backup = Primary + 65536U;
  UINT8 *Entries = Backup + 65536U, *OtherEntries = Entries + DALI_UFS_GPT_MAX_BYTES;
  Status = ReadOnce (Read, Context, Expected->Lun, 1, G.BlockSize, Primary, &Geometry[Expected->Lun], &G);
  if (Status != EFI_SUCCESS) { return Status; }
  Status = ReadOnce (Read, Context, Expected->Lun, G.Blocks - 1, G.BlockSize, Backup, &Geometry[Expected->Lun], &G);
  if (Status != EFI_SUCCESS) { return Status; }
  GPT_HEADER A, B;
  if (!Header (Primary, &G, TRUE, &A) || !Header (Backup, &G, FALSE, &B) ||
      A.First != B.First || A.Last != B.Last || A.Count != B.Count || A.Stride != B.Stride ||
      A.EntriesCrc != B.EntriesCrc || CompareMem (A.DiskGuid, B.DiskGuid, sizeof A.DiskGuid)) { return EFI_VOLUME_CORRUPTED; }
  UINTN EntryBytes = (UINTN)A.Count * A.Stride;
  UINTN ReadBytes = (EntryBytes + G.BlockSize - 1) & ~((UINTN)G.BlockSize - 1);
  Status = ReadOnce (Read, Context, Expected->Lun, A.EntriesLba, ReadBytes, Entries, &Geometry[Expected->Lun], &G);
  if (Status != EFI_SUCCESS) { return Status; }
  Status = ReadOnce (Read, Context, Expected->Lun, B.EntriesLba, ReadBytes, OtherEntries, &Geometry[Expected->Lun], &G);
  if (Status != EFI_SUCCESS) { return Status; }
  if (Crc32 (Entries, EntryBytes, FALSE) != A.EntriesCrc ||
      Crc32 (OtherEntries, EntryBytes, FALSE) != B.EntriesCrc ||
      CompareMem (Entries, OtherEntries, EntryBytes)) { return EFI_VOLUME_CORRUPTED; }
  CONST UINT8 *Boot = NULL;
  for (UINTN I = 0; I < A.Count; ++I) {
    CONST UINT8 *P = Entries + I * A.Stride;
    if (!Nonzero (P, 16)) { continue; }
    UINT64 First = Le64 (P + 32), Last = Le64 (P + 40);
    if (!Nonzero (P + 16, 16) || First < A.First || Last > A.Last || First > Last) { return EFI_VOLUME_CORRUPTED; }
    for (UINTN J = 0; J < I; ++J) {
      CONST UINT8 *Q = Entries + J * A.Stride;
      if (Nonzero (Q, 16) && (CompareMem (P + 16, Q + 16, 16) == 0 ||
          (First <= Le64 (Q + 40) && Le64 (Q + 32) <= Last))) { return EFI_VOLUME_CORRUPTED; }
    }
    if (TargetName (P, Expected->Target)) {
      UINTN End = Expected->Target == MT6991_UFS_TARGET_EXPDB ? 68 :
                  Expected->Target == MT6991_UFS_TARGET_USERDATA_LOGS ? 80 : 70;
      if (Boot != NULL || Nonzero (P + End, 128 - End)) { return EFI_VOLUME_CORRUPTED; }
      Boot = P;
    }
  }
  if (Boot == NULL) { return EFI_NOT_FOUND; }
  if (Le64 (Boot + 48) & (1ULL << 60)) { return EFI_WRITE_PROTECTED; }
  DALI_UFS_LAYOUT L;
  ZeroMem (&L, sizeof L);
  L.Lun = Expected->Lun; L.BlockSize = G.BlockSize; L.CapacityBlocks = G.Blocks;
  L.Target = Expected->Target;
  L.BootStartLba = Le64 (Boot + 32); L.BootBlocks = Le64 (Boot + 40) - L.BootStartLba + 1;
  Guid (Boot + 16, L.PartitionGuid);
  L.ScratchBlocks = L.TailGuardBlocks = (UINT64)(Expected->Target == MT6991_UFS_TARGET_USERDATA_LOGS ?
    MT6991_UFS_DATA_LOG_SLOT_BYTES : 1024U * 1024U) / G.BlockSize;
  if (L.BootBlocks < 2 * L.ScratchBlocks) { return EFI_ACCESS_DENIED; }
  if (L.Target == MT6991_UFS_TARGET_EXPDB) {
    if (L.BootBlocks < 6 * L.ScratchBlocks) { return EFI_ACCESS_DENIED; }
    L.ScratchStartLba = L.BootStartLba + L.ScratchBlocks;
    L.TailGuardBlocks = L.BootBlocks - 2 * L.ScratchBlocks;
  } else if (L.Target == MT6991_UFS_TARGET_USERDATA_LOGS) {
    if (L.BootBlocks != 8192ULL || Expected->Sequence == 0 || Expected->Sequence > 4 ||
        L.BlockSize != 4096 || L.ScratchBlocks != MT6991_UFS_DATA_LOG_SLOT_BYTES / L.BlockSize) {
      return EFI_ACCESS_DENIED;
    }
    UINT64 SlotOffset = (Expected->Sequence - 1) * 2 * L.ScratchBlocks;
    L.ScratchStartLba = L.BootStartLba + SlotOffset;
    L.TailGuardBlocks = L.BootBlocks - SlotOffset - L.ScratchBlocks;
  } else { L.ScratchStartLba = L.BootStartLba + L.BootBlocks - 2 * L.ScratchBlocks; }
  if (L.BootStartLba != Expected->BootStartLba || L.BootBlocks != Expected->BootBlocks ||
      CompareMem (L.PartitionGuid, Expected->PartitionGuid, sizeof L.PartitionGuid) ||
      L.ScratchStartLba != Expected->ScratchStartLba || L.ScratchBlocks != Expected->ScratchBlocks ||
      L.TailGuardBlocks != Expected->TailGuardBlocks) { return EFI_SECURITY_VIOLATION; }
  if (!SameGeometry (&Geometry[Expected->Lun], &G)) { return EFI_MEDIA_CHANGED; }
  L.EntryCount = A.Count; L.EntryCrc32 = A.EntriesCrc; L.PrimaryCrc32 = A.HeaderCrc; L.BackupCrc32 = B.HeaderCrc;
  L.Verified = TRUE; CopyMem (Observed, &L, sizeof L);
  return EFI_SUCCESS;
}
