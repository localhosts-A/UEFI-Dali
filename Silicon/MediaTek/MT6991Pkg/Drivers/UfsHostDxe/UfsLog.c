// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include "UfsLog.h"

STATIC CONST UINT8 MagicV1[16] = "DALIUFSLOG1";
STATIC CONST UINT8 MagicV2[16] = "DALIUFSLOG2";
enum { Kind = 16, Version = 20, Token = 24, OwnerAt = 56, SequenceAt = 60,
  LengthAt = 68, BlockAt = 72, BlocksAt = 76, DeviceAt = 84,
  CandidateAt = 116, SourceAt = 148, BodyHashAt = 180, RecordHashAt = 212,
  GenerationAt = 244, FlagsAt = 252, CaptureStageAt = 256, BodyFormatAt = 260 };

STATIC VOID Put (UINT8 *P, UINT64 V, UINTN Bytes) {
  for (UINTN I = 0; I < Bytes; ++I) { P[I] = (UINT8)(V >> (I * 8)); }
}
STATIC UINT64 Get (CONST UINT8 *P, UINTN Bytes) {
  UINT64 V = 0; for (UINTN I = 0; I < Bytes; ++I) { V |= (UINT64)P[I] << (I * 8); } return V;
}
STATIC BOOLEAN Zero (CONST UINT8 *P, UINTN Bytes) {
  for (UINTN I = 0; I < Bytes; ++I) { if (P[I] != 0) { return FALSE; } } return TRUE;
}
STATIC BOOLEAN ValidToken (CONST UINT8 *P) {
  if (P == NULL) { return FALSE; }
  for (UINTN I = 0; I < 32; ++I) {
    if (!((P[I] >= '0' && P[I] <= '9') || (P[I] >= 'a' && P[I] <= 'f'))) { return FALSE; }
  }
  return TRUE;
}
STATIC BOOLEAN SameRecordContent (CONST UINT8 *A, CONST UINT8 *B, UINT32 Block) {
  for (UINTN I = 0; I < Block; ++I) {
    if ((I >= Kind && I < Kind + 4) ||
        (I >= RecordHashAt && I < RecordHashAt + 32)) { continue; }
    if (A[I] != B[I]) { return FALSE; }
  }
  return TRUE;
}

STATIC EFI_STATUS VerifyReplacement (CONST UINT8 *Read,
  CONST MT6991_UFS_SCRATCH_GRANT *G, UINT64 Sequence,
  CONST DALI_UFS_LOG_V2_IDENTITY *Identity, UINTN Window) {
  UINT32 Block = G->BlockSize;
  if (Identity == NULL || !Identity->Valid || Identity->Generation == 0 ||
      !ValidToken (Identity->Token) || Block != 4096 || Window != DALI_UFS_LOG_USERDATA_SLOT_BYTES ||
      Sequence != G->Sequence) { return EFI_SECURITY_VIOLATION; }
  UINT32 BodyBytes = (UINT32)Get (Read + Block + LengthAt, 4);
  if (BodyBytes == 0 || BodyBytes > Window - 3U * Block) { return EFI_COMPROMISED_DATA; }

  CONST UINTN RecordOffsets[3] = { 0, Block, Window - Block };
  CONST UINT8 *Records[3];
  for (UINTN I = 0; I < 3; ++I) {
    UINT8 Copy[4096], Digest[32], Stored[32];
    CONST UINT8 *RecordBytes = Read + RecordOffsets[I];
    Records[I] = RecordBytes;
    if (CompareMem (RecordBytes, MagicV2, sizeof MagicV2) != 0 ||
        Get (RecordBytes + Kind, 4) != I + 1 || Get (RecordBytes + Version, 4) != 2 ||
        CompareMem (RecordBytes + Token, Identity->Token, sizeof Identity->Token) != 0 ||
        Get (RecordBytes + OwnerAt, 4) != DALI_UFS_LOG_OWNER_UEFI ||
        Get (RecordBytes + SequenceAt, 8) != Sequence ||
        Get (RecordBytes + LengthAt, 4) != BodyBytes ||
        Get (RecordBytes + BlockAt, 4) != Block ||
        Get (RecordBytes + BlocksAt, 8) != G->ScratchBlocks ||
        CompareMem (RecordBytes + DeviceAt, G->DeviceBinding, 32) != 0 ||
        CompareMem (RecordBytes + CandidateAt, Identity->CandidateSha256, 32) != 0 ||
        CompareMem (RecordBytes + SourceAt, Identity->SourceSha256, 32) != 0 ||
        CompareMem (RecordBytes + BodyHashAt, Identity->BodySha256, 32) != 0 ||
        Get (RecordBytes + GenerationAt, 8) != Identity->Generation ||
        Get (RecordBytes + FlagsAt, 4) != 0 ||
        Get (RecordBytes + CaptureStageAt, 4) != 1 ||
        Get (RecordBytes + BodyFormatAt, 4) != 1 ||
        !Zero (RecordBytes + BodyFormatAt + 4, Block - (BodyFormatAt + 4))) {
      return EFI_COMPROMISED_DATA;
    }
    CopyMem (Copy, RecordBytes, Block);
    CopyMem (Stored, Copy + RecordHashAt, sizeof Stored);
    ZeroMem (Copy + RecordHashAt, sizeof Stored);
    if (!Sha256HashAll (Copy, Block, Digest)) { return EFI_DEVICE_ERROR; }
    if (CompareMem (Digest, Stored, sizeof Digest) != 0) { return EFI_COMPROMISED_DATA; }
  }
  if (!SameRecordContent (Records[0], Records[1], Block) ||
      !SameRecordContent (Records[1], Records[2], Block)) { return EFI_COMPROMISED_DATA; }

  UINT8 BodyDigest[32];
  if (!Sha256HashAll (Read + 2U * Block, BodyBytes, BodyDigest)) { return EFI_DEVICE_ERROR; }
  if (CompareMem (BodyDigest, Identity->BodySha256, sizeof BodyDigest) != 0 ||
      !Zero (Read + 2U * Block + BodyBytes, Window - 3U * Block - BodyBytes)) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Exact (EFI_STATUS Status) {
  return Status == EFI_SUCCESS || EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
}
STATIC UINTN WindowBytes (CONST MT6991_UFS_SCRATCH_GRANT *G) {
  return G->Target == MT6991_UFS_TARGET_USERDATA_LOGS ? DALI_UFS_LOG_USERDATA_SLOT_BYTES :
         DALI_UFS_LOG_WINDOW_BYTES;
}

STATIC BOOLEAN Record (UINT8 *P, UINT32 Block, UINT32 Type,
  CONST MT6991_UFS_SCRATCH_GRANT *G, CONST UINT8 *RecordToken,
  UINT32 Owner, UINT64 Sequence, UINT64 Generation,
  UINT32 Bytes, CONST UINT8 Digest[32]) {
  BOOLEAN IsV2 = Generation != 0;
  ZeroMem (P, Block); CopyMem (P, IsV2 ? MagicV2 : MagicV1, sizeof MagicV1);
  Put (P + Kind, Type, 4); Put (P + Version, IsV2 ? 2 : 1, 4); CopyMem (P + Token, RecordToken, 32);
  Put (P + OwnerAt, Owner, 4); Put (P + SequenceAt, Sequence, 8);
  Put (P + LengthAt, Bytes, 4); Put (P + BlockAt, Block, 4); Put (P + BlocksAt, G->ScratchBlocks, 8);
  CopyMem (P + DeviceAt, G->DeviceBinding, 32); CopyMem (P + CandidateAt, G->CandidateSha256, 32);
  CopyMem (P + SourceAt, G->SourceSha256, 32); CopyMem (P + BodyHashAt, Digest, 32);
  if (IsV2) {
    Put (P + GenerationAt, Generation, 8);
    Put (P + FlagsAt, 0, 4);
    Put (P + CaptureStageAt, Owner == DALI_UFS_LOG_OWNER_UEFI ? 1 : 2, 4);
    Put (P + BodyFormatAt, Owner == DALI_UFS_LOG_OWNER_UEFI ? 1 : 2, 4);
  }
  UINT8 Self[32];
  if (!Sha256HashAll (P, Block, Self)) { return FALSE; }
  CopyMem (P + RecordHashAt, Self, 32); return TRUE;
}

STATIC EFI_STATUS Verify (UINT8 *Read, UINT8 *Expected,
  CONST MT6991_UFS_SCRATCH_GRANT *G, CONST UINT8 *RecordToken,
  UINT32 Owner, UINT64 Sequence, UINT64 Generation,
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
    if (!Record (Expected + At, Block, I + 1, G, RecordToken, Owner, Sequence, Generation,
        (UINT32)Length, Digest)) { return EFI_DEVICE_ERROR; }
    if (CompareMem (Expected + At, Read + At, Block)) { return EFI_COMPROMISED_DATA; }
  }
  if (!Zero (Read + 2 * Block + (UINTN)Length,
      Window - 3 * Block - (UINTN)Length)) { return EFI_COMPROMISED_DATA; }
  Result->Body = Read + 2 * Block; Result->BodyBytes = (UINT32)Length;
  CopyMem (Result->BodySha256, Digest, 32); Result->Verified = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS DaliUfsLogRun (EFI_BLOCK_IO_PROTOCOL *Io,
  CONST MT6991_UFS_SCRATCH_GRANT *G, UINT32 Owner, UINT64 Sequence, UINT64 Generation,
  CONST UINT8 *RecordToken, CONST DALI_UFS_LOG_V2_IDENTITY *Replacement,
  BOOLEAN ReadbackOnly,
  CONST UINT8 *Body, UINTN BodyBytes,
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
  if (Generation != 0 && G->Target != MT6991_UFS_TARGET_USERDATA_LOGS) {
    return EFI_SECURITY_VIOLATION;
  }
  if (G->Target == MT6991_UFS_TARGET_USERDATA_LOGS && !ReadbackOnly && Generation == 0) {
    return EFI_SECURITY_VIOLATION;
  }
  if (Generation != 0 && RecordToken == NULL) { return EFI_SECURITY_VIOLATION; }
  if (!ReadbackOnly && RecordToken == NULL) { return EFI_SECURITY_VIOLATION; }
  if (Replacement != NULL &&
      (ReadbackOnly || G->Target != MT6991_UFS_TARGET_USERDATA_LOGS ||
       Owner != DALI_UFS_LOG_OWNER_UEFI || !Replacement->Valid ||
       Replacement->Generation == 0 || Replacement->Generation >= Generation ||
       Sequence != G->Sequence)) { return EFI_SECURITY_VIOLATION; }
  if (RecordToken == NULL) { RecordToken = (CONST UINT8 *)G->Token; }
  if (!ValidToken (RecordToken)) { return EFI_SECURITY_VIOLATION; }
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
      if (!Record (Image + At, Block, I + 1, G, RecordToken, Owner, Sequence, Generation,
          (UINT32)BodyBytes, Digest)) { return EFI_DEVICE_ERROR; }
    }
  }
  EFI_STATUS Status = Exact (Io->ReadBlocks (Io, Media.MediaId, 0, Window, Read));
  if (Status != EFI_SUCCESS) { return Status; }
  if (ReadbackOnly) { return Verify (Read, Image, G, RecordToken, Owner, Sequence, Generation, Window, Result); }
  if (Replacement == NULL) {
    if (!Zero (Read, Window)) { return EFI_ALREADY_STARTED; }
  } else {
    Status = VerifyReplacement (Read, G, Sequence, Replacement, Window);
    if (Status != EFI_SUCCESS) { return Status; }
    // Replacement is intentionally destructive only after a second complete
    // validation of the observed victim. Clear every block so old body/padding
    // cannot contaminate a shorter replacement record.
    ZeroMem (Read, Window);
    Result->WriteSubmitted = TRUE;
    Status = Exact (Io->WriteBlocks (Io, Media.MediaId, 0, Window, Read));
    if (Status != EFI_SUCCESS) { return Status; }
    Status = Exact (Io->FlushBlocks (Io));
    if (Status != EFI_SUCCESS) { return Status; }
    Status = Exact (Io->ReadBlocks (Io, Media.MediaId, 0, Window, Read));
    if (Status != EFI_SUCCESS) { return Status; }
    if (!Zero (Read, Window)) { return EFI_COMPROMISED_DATA; }
  }
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
  return Verify (Read, Image, G, RecordToken, Owner, Sequence, Generation, Window, Result);
}
