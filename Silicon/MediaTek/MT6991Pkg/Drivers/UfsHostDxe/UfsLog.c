// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include "UfsLog.h"

STATIC CONST UINT8 Magic[16] = "DALIUFSLOG1";
enum { Kind = 16, Version = 20, Token = 24, OwnerAt = 56, SequenceAt = 60,
  LengthAt = 68, BlockAt = 72, BlocksAt = 76, DeviceAt = 84,
  CandidateAt = 116, SourceAt = 148, BodyHashAt = 180, RecordHashAt = 212 };

STATIC VOID Put (UINT8 *P, UINT64 V, UINTN Bytes) {
  for (UINTN I = 0; I < Bytes; ++I) { P[I] = (UINT8)(V >> (I * 8)); }
}
STATIC UINT64 Get (CONST UINT8 *P, UINTN Bytes) {
  UINT64 V = 0; for (UINTN I = 0; I < Bytes; ++I) { V |= (UINT64)P[I] << (I * 8); } return V;
}
STATIC BOOLEAN Zero (CONST UINT8 *P, UINTN Bytes) {
  for (UINTN I = 0; I < Bytes; ++I) { if (P[I] != 0) { return FALSE; } } return TRUE;
}
STATIC EFI_STATUS Exact (EFI_STATUS Status) {
  return Status == EFI_SUCCESS || EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
}
STATIC UINTN WindowBytes (CONST MT6991_UFS_SCRATCH_GRANT *G) {
  return G->Target == MT6991_UFS_TARGET_USERDATA_LOGS ? DALI_UFS_LOG_USERDATA_SLOT_BYTES :
         DALI_UFS_LOG_WINDOW_BYTES;
}

STATIC BOOLEAN Record (UINT8 *P, UINT32 Block, UINT32 Type,
  CONST MT6991_UFS_SCRATCH_GRANT *G, UINT32 Owner, UINT64 Sequence,
  UINT32 Bytes, CONST UINT8 Digest[32]) {
  ZeroMem (P, Block); CopyMem (P, Magic, sizeof Magic);
  Put (P + Kind, Type, 4); Put (P + Version, 1, 4); CopyMem (P + Token, G->Token, 32);
  Put (P + OwnerAt, Owner, 4); Put (P + SequenceAt, Sequence, 8);
  Put (P + LengthAt, Bytes, 4); Put (P + BlockAt, Block, 4); Put (P + BlocksAt, G->ScratchBlocks, 8);
  CopyMem (P + DeviceAt, G->DeviceBinding, 32); CopyMem (P + CandidateAt, G->CandidateSha256, 32);
  CopyMem (P + SourceAt, G->SourceSha256, 32); CopyMem (P + BodyHashAt, Digest, 32);
  UINT8 Self[32];
  if (!Sha256HashAll (P, Block, Self)) { return FALSE; }
  CopyMem (P + RecordHashAt, Self, 32); return TRUE;
}

STATIC EFI_STATUS Verify (UINT8 *Read, UINT8 *Expected,
  CONST MT6991_UFS_SCRATCH_GRANT *G, UINT32 Owner, UINT64 Sequence,
  UINTN Window, DALI_UFS_LOG_RESULT *Result) {
  UINT32 Block = G->BlockSize;
  CONST UINT8 *Header = Read + Block;
  UINT64 Length = Get (Header + LengthAt, 4);
  if (Length == 0 || Length > Window - 3 * Block) { return EFI_COMPROMISED_DATA; }
  UINT8 Digest[32];
  if (!Sha256HashAll (Read + 2 * Block, (UINTN)Length, Digest)) { return EFI_DEVICE_ERROR; }
  // Reconstruct every record rather than trusting its claimed self hash.
  ZeroMem (Expected, Window);
  for (UINT32 I = 0; I < 3; ++I) {
    UINTN At = I == 2 ? Window - Block : I * Block;
    if (!Record (Expected + At, Block, I + 1, G, Owner, Sequence, (UINT32)Length, Digest)) { return EFI_DEVICE_ERROR; }
    if (CompareMem (Expected + At, Read + At, Block)) { return EFI_COMPROMISED_DATA; }
  }
  if (!Zero (Read + 2 * Block + (UINTN)Length,
      Window - 3 * Block - (UINTN)Length)) { return EFI_COMPROMISED_DATA; }
  Result->Body = Read + 2 * Block; Result->BodyBytes = (UINT32)Length;
  CopyMem (Result->BodySha256, Digest, 32); Result->Verified = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS DaliUfsLogRun (EFI_BLOCK_IO_PROTOCOL *Io,
  CONST MT6991_UFS_SCRATCH_GRANT *G, UINT32 Owner, UINT64 Sequence,
  BOOLEAN ReadbackOnly, CONST UINT8 *Body, UINTN BodyBytes,
  VOID *Workspace, UINTN WorkspaceBytes, DALI_UFS_LOG_RESULT *Result) {
  if (Result == NULL) { return EFI_INVALID_PARAMETER; }
  if (Result->Attempted) { return EFI_ALREADY_STARTED; }
  if (G == NULL || !Mt6991UfsScratchGrantValid (G) ||
      (G->Target != MT6991_UFS_TARGET_EXPDB && G->Target != MT6991_UFS_TARGET_USERDATA_LOGS) ||
      (Owner != DALI_UFS_LOG_OWNER_UEFI && Owner != DALI_UFS_LOG_OWNER_XNU) || Sequence == 0 ||
      Io == NULL || Io->Media == NULL || Io->ReadBlocks == NULL || Io->FlushBlocks == NULL ||
      (!ReadbackOnly && Io->WriteBlocks == NULL) || Workspace == NULL || WorkspaceBytes < DALI_UFS_LOG_WORK_BYTES) {
    return EFI_INVALID_PARAMETER;
  }
  EFI_BLOCK_IO_MEDIA Media = *Io->Media;
  UINT32 Block = G->BlockSize, Align = Media.IoAlign;
  UINTN Window = WindowBytes (G);
  if (G->Target == MT6991_UFS_TARGET_USERDATA_LOGS &&
      (Owner != DALI_UFS_LOG_OWNER_UEFI || Sequence != G->Sequence)) { return EFI_SECURITY_VIOLATION; }
  if (!Media.MediaPresent || !Media.LogicalPartition || Media.BlockSize != Block ||
      Window % Block || G->ScratchBlocks != Window / Block || Media.LastBlock != G->ScratchBlocks - 1 ||
      Align > EFI_PAGE_SIZE || (Align != 0 && (Align & (Align - 1)) != 0) ||
      (Align > 1 && ((UINTN)Workspace & (Align - 1)) != 0) || (!ReadbackOnly && Media.ReadOnly)) {
    return EFI_ACCESS_DENIED;
  }
  if (ReadbackOnly ? (Body != NULL || BodyBytes != 0) :
      (Body == NULL || BodyBytes == 0 || BodyBytes > Window - 3 * Block)) { return EFI_BAD_BUFFER_SIZE; }
  // Body must be a distinct frozen RAM snapshot, never the readback buffer.
  UINTN Start = (UINTN)Workspace, End = Start + DALI_UFS_LOG_WORK_BYTES, Input = (UINTN)Body;
  if (End < Start || (!ReadbackOnly && (Input > MAX_UINTN - BodyBytes || (Input < End && Input + BodyBytes > Start)))) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Result, sizeof *Result); Result->Attempted = TRUE;
  UINT8 *Image = Workspace, *Read = Image + Window;
  if (!ReadbackOnly) {
    UINT8 Digest[32];
    ZeroMem (Image, Window); CopyMem (Image + 2 * Block, Body, BodyBytes);
    if (!Sha256HashAll (Image + 2 * Block, BodyBytes, Digest)) { return EFI_DEVICE_ERROR; }
    for (UINT32 I = 0; I < 3; ++I) {
      UINTN At = I == 2 ? Window - Block : I * Block;
      if (!Record (Image + At, Block, I + 1, G, Owner, Sequence, (UINT32)BodyBytes, Digest)) { return EFI_DEVICE_ERROR; }
    }
  }
  EFI_STATUS Status = Exact (Io->ReadBlocks (Io, Media.MediaId, 0, Window, Read));
  if (Status != EFI_SUCCESS) { return Status; }
  if (ReadbackOnly) { return Verify (Read, Image, G, Owner, Sequence, Window, Result); }
  if (!Zero (Read, Window)) { return EFI_ALREADY_STARTED; }
  for (UINT32 Phase = 0; Phase < 3; ++Phase) {
    UINT64 Lba = Phase == 2 ? G->ScratchBlocks - 1 : Phase;
    UINTN Bytes = Phase == 1 ? Window - 2 * Block : Block;
    Result->WriteSubmitted = TRUE;
    Status = Exact (Io->WriteBlocks (Io, Media.MediaId, Lba, Bytes, Image + (UINTN)Lba * Block));
    if (Status != EFI_SUCCESS) { return Status; }
    Status = Exact (Io->FlushBlocks (Io));
    if (Status != EFI_SUCCESS) { return Status; }
  }
  Result->CommitSynced = TRUE;
  Status = Exact (Io->ReadBlocks (Io, Media.MediaId, 0, Window, Read));
  if (Status != EFI_SUCCESS) { return Status; }
  if (CompareMem (Read, Image, Window)) { return EFI_COMPROMISED_DATA; }
  return Verify (Read, Image, G, Owner, Sequence, Window, Result);
}
