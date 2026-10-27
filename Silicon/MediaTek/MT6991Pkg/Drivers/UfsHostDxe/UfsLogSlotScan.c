// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "UfsLogSlotScan.h"

#define LOG_WINDOW_BYTES (4U * 1024U * 1024U)
#define LOG_OWNER_UEFI 1U
#define LOG_OWNER_XNU 2U
#define XNU_BODY_HEADER_BYTES 64U
#define XNU_MSGBUF_CAPACITY (1024U * 1024U)
#define XNU_CONSOLE_CAPACITY (2U * 1024U * 1024U - 12U)

enum {
  LOG_KIND = 16, LOG_VERSION = 20, LOG_TOKEN = 24, LOG_OWNER = 56,
  LOG_SEQUENCE = 60, LOG_LENGTH = 68, LOG_BLOCK_SIZE = 72,
  LOG_BLOCKS = 76, LOG_DEVICE = 84, LOG_CANDIDATE = 116,
  LOG_SOURCE = 148, LOG_BODY_HASH = 180, LOG_RECORD_HASH = 212,
  LOG_V2_GENERATION = 244, LOG_V2_FLAGS = 252,
  LOG_V2_CAPTURE_STAGE = 256, LOG_V2_BODY_FORMAT = 260,
  LOG_V1_END = 244, LOG_V2_END = 264
};

STATIC CONST UINT8 mMagicV1[16] = "DALIUFSLOG1";
STATIC CONST UINT8 mMagicV2[16] = "DALIUFSLOG2";
STATIC CONST UINT8 mPartitionGuid[16] = {
  0xcd, 0xe9, 0xab, 0x32, 0x18, 0xd8, 0x47, 0x77,
  0x8c, 0xed, 0x69, 0x1e, 0x7c, 0xd5, 0xc6, 0x36
};

typedef struct {
  DALI_UFS_LOG_SLOT_STATE State;
  UINT32 Owner;
  UINT64 Sequence;
  UINT64 Generation;
  UINT8 Token[32];
  UINT8 DeviceBinding[32];
  UINT8 CandidateSha256[32];
  UINT8 SourceSha256[32];
} DALI_SLOT_INFO;

STATIC UINT32 Le32 (CONST UINT8 *P) {
  return (UINT32)P[0] | (UINT32)P[1] << 8 | (UINT32)P[2] << 16 | (UINT32)P[3] << 24;
}

STATIC UINT64 Le64 (CONST UINT8 *P) {
  return (UINT64)Le32 (P) | (UINT64)Le32 (P + 4) << 32;
}

STATIC BOOLEAN ZeroBytes (CONST UINT8 *Bytes, UINTN Length) {
  UINT8 Any = 0;
  for (UINTN I = 0; I < Length; ++I) { Any |= Bytes[I]; }
  return Any == 0;
}

STATIC BOOLEAN ValidToken (CONST UINT8 *Token) {
  if (Token == NULL) { return FALSE; }
  for (UINTN I = 0; I < 32; ++I) {
    if (!((Token[I] >= '0' && Token[I] <= '9') ||
          (Token[I] >= 'a' && Token[I] <= 'f'))) { return FALSE; }
  }
  return TRUE;
}

STATIC UINT32 IdentifySchema (CONST UINT8 *Record) {
  if (CompareMem (Record, mMagicV1, sizeof mMagicV1) == 0 &&
      Le32 (Record + LOG_VERSION) == 1) { return 1; }
  if (CompareMem (Record, mMagicV2, sizeof mMagicV2) == 0 &&
      Le32 (Record + LOG_VERSION) == 2) { return 2; }
  return 0;
}

STATIC EFI_STATUS VerifyRecordBlock (CONST UINT8 *Record, UINT32 BlockSize,
  UINT32 Kind, UINT32 *Schema) {
  UINT32 Version = IdentifySchema (Record);
  if (Schema == NULL || Version == 0 || Le32 (Record + LOG_KIND) != Kind ||
      Le32 (Record + LOG_BLOCK_SIZE) != BlockSize) { return EFI_COMPROMISED_DATA; }
  UINTN Used = Version == 1 ? LOG_V1_END : LOG_V2_END;
  if (Used > BlockSize || !ZeroBytes (Record + Used, BlockSize - Used)) {
    return EFI_COMPROMISED_DATA;
  }
  UINT8 Copy[4096], Digest[SHA256_DIGEST_SIZE], Stored[SHA256_DIGEST_SIZE];
  if (BlockSize != sizeof Copy) { return EFI_COMPROMISED_DATA; }
  CopyMem (Copy, Record, BlockSize);
  CopyMem (Stored, Record + LOG_RECORD_HASH, sizeof Stored);
  ZeroMem (Copy + LOG_RECORD_HASH, sizeof Stored);
  if (!Sha256HashAll (Copy, BlockSize, Digest)) { return EFI_DEVICE_ERROR; }
  if (CompareMem (Digest, Stored, sizeof Digest)) { return EFI_COMPROMISED_DATA; }
  *Schema = Version;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ValidateRecordFields (CONST UINT8 *Record, UINT32 BlockSize,
  UINT64 SlotBlocks, UINT32 SlotOwner, UINT64 SlotSequence,
  CONST MT6991_UFS_SCRATCH_GRANT *Expected, DALI_SLOT_INFO *Info) {
  UINT32 Schema = IdentifySchema (Record);
  UINT64 Length = Le32 (Record + LOG_LENGTH);
  if (Schema == 0 || Le32 (Record + LOG_BLOCK_SIZE) != BlockSize ||
      Le64 (Record + LOG_BLOCKS) != SlotBlocks ||
      Le32 (Record + LOG_OWNER) != SlotOwner ||
      Le64 (Record + LOG_SEQUENCE) != SlotSequence ||
      Length == 0 || Length > LOG_WINDOW_BYTES - 3U * BlockSize ||
      !ValidToken (Record + LOG_TOKEN)) { return EFI_COMPROMISED_DATA; }
  if (Expected != NULL &&
      CompareMem (Record + LOG_DEVICE, Expected->DeviceBinding, sizeof Expected->DeviceBinding)) {
    return EFI_COMPROMISED_DATA;
  }
  if (ZeroBytes (Record + LOG_DEVICE, 32) ||
      ZeroBytes (Record + LOG_CANDIDATE, 32) ||
      ZeroBytes (Record + LOG_SOURCE, 32)) { return EFI_COMPROMISED_DATA; }
  if (Schema == 2) {
    UINT64 Generation = Le64 (Record + LOG_V2_GENERATION);
    UINT32 Flags = Le32 (Record + LOG_V2_FLAGS);
    UINT32 Stage = Le32 (Record + LOG_V2_CAPTURE_STAGE);
    UINT32 Format = Le32 (Record + LOG_V2_BODY_FORMAT);
    if (Generation == 0 || (Flags & ~1U) != 0 ||
        (SlotOwner == LOG_OWNER_UEFI && (Stage != 1 || Format != 1)) ||
        (SlotOwner == LOG_OWNER_XNU && (Stage != 2 || Format != 2))) {
      return EFI_COMPROMISED_DATA;
    }
    Info->Generation = Generation;
  }
  Info->Owner = SlotOwner;
  Info->Sequence = SlotSequence;
  CopyMem (Info->Token, Record + LOG_TOKEN, sizeof Info->Token);
  CopyMem (Info->DeviceBinding, Record + LOG_DEVICE, sizeof Info->DeviceBinding);
  CopyMem (Info->CandidateSha256, Record + LOG_CANDIDATE, sizeof Info->CandidateSha256);
  CopyMem (Info->SourceSha256, Record + LOG_SOURCE, sizeof Info->SourceSha256);
  return EFI_SUCCESS;
}

STATIC BOOLEAN ValidXnuBodyHeader (CONST UINT8 *Body, UINT64 BodyBytes) {
  STATIC CONST UINT8 Magic[] = "DALI-XNU-BODY1";
  if (Body == NULL || BodyBytes < XNU_BODY_HEADER_BYTES ||
      CompareMem (Body, Magic, sizeof Magic - 1) != 0) { return FALSE; }
  UINT32 Version = Le32 (Body + 16);
  UINT32 HeaderBytes = Le32 (Body + 20);
  UINT32 DiagnosticBytes = Le32 (Body + 24);
  UINT32 MessageBytes = Le32 (Body + 28);
  UINT32 MessageSize = Le32 (Body + 32);
  UINT32 MessageWrite = Le32 (Body + 36);
  UINT32 MessageRead = Le32 (Body + 40);
  UINT32 ConsoleBytes = Le32 (Body + 44);
  UINT32 ConsoleCapacity = Le32 (Body + 48);
  UINT32 TotalBytes = Le32 (Body + 60);
  UINT64 ExpectedBytes = (UINT64)HeaderBytes + DiagnosticBytes + MessageBytes + ConsoleBytes;
  return Version == 1 && HeaderBytes == XNU_BODY_HEADER_BYTES &&
    DiagnosticBytes != 0 && DiagnosticBytes <= XNU_MSGBUF_CAPACITY &&
    MessageBytes == MessageSize && MessageSize != 0 && MessageSize <= XNU_MSGBUF_CAPACITY &&
    MessageWrite < MessageSize && MessageRead < MessageSize &&
    ConsoleCapacity == XNU_CONSOLE_CAPACITY && ConsoleBytes <= ConsoleCapacity &&
    TotalBytes == BodyBytes && ExpectedBytes == BodyBytes;
}

STATIC BOOLEAN SameRecordContent (CONST UINT8 *A, CONST UINT8 *B, UINT32 BlockSize) {
  for (UINTN I = 0; I < BlockSize; ++I) {
    if ((I >= LOG_KIND && I < LOG_KIND + 4) ||
        (I >= LOG_RECORD_HASH && I < LOG_RECORD_HASH + SHA256_DIGEST_SIZE)) { continue; }
    if (A[I] != B[I]) { return FALSE; }
  }
  return TRUE;
}

STATIC BOOLEAN SameRecordExceptKindAndDigest (CONST UINT8 *A,
  CONST UINT8 *B, CONST UINT8 *C, UINT32 BlockSize) {
  return SameRecordContent (A, B, BlockSize) && SameRecordContent (B, C, BlockSize);
}

STATIC EFI_STATUS ReadBounded (CONST DALI_UFS_LAYOUT *Layout,
  DALI_UFS_LAYOUT_READ Read, VOID *Context, VOID *Buffer,
  UINT64 SlotOffsetBlocks, UINT64 ByteOffset, UINTN Bytes,
  UINT64 PartitionEnd) {
  UINT64 Block = Layout->BlockSize;
  if (Bytes == 0 || Bytes > MT6991_UFS_DMA_MAX_TRANSFER || Bytes % Block != 0 ||
      ByteOffset % Block != 0) { return EFI_BAD_BUFFER_SIZE; }
  UINT64 Relative = SlotOffsetBlocks + ByteOffset / Block;
  UINT64 Count = Bytes / Block;
  if (Relative < SlotOffsetBlocks || Relative > Layout->BootBlocks ||
      Count > Layout->BootBlocks - Relative) { return EFI_SECURITY_VIOLATION; }
  UINT64 Lba = Layout->BootStartLba + Relative;
  if (Lba < Layout->BootStartLba || Lba > PartitionEnd ||
      Count > PartitionEnd - Lba) { return EFI_SECURITY_VIOLATION; }
  return Read (Context, Layout->Lun, Lba, Bytes, Buffer);
}

STATIC EFI_STATUS VerifyBody (CONST DALI_UFS_LAYOUT *Layout,
  DALI_UFS_LAYOUT_READ Read, VOID *Context, VOID *Buffer, VOID *HashContext,
  UINT64 SlotOffsetBlocks, UINT64 PartitionEnd,
  CONST UINT8 *Record, UINT8 Digest[SHA256_DIGEST_SIZE], BOOLEAN *ReadFailure) {
  UINT32 BlockSize = Layout->BlockSize;
  UINT64 BodyBytes = Le32 (Record + LOG_LENGTH);
  UINT64 PayloadRegionBytes = LOG_WINDOW_BYTES - 3U * BlockSize;
  BOOLEAN ValidateXnu = IdentifySchema (Record) == 2 &&
    Le32 (Record + LOG_V2_BODY_FORMAT) == 2;
  EFI_STATUS Status;
  if (ReadFailure == NULL) { return EFI_INVALID_PARAMETER; }
  *ReadFailure = FALSE;
  if (BodyBytes == 0 || BodyBytes > PayloadRegionBytes || !Sha256Init (HashContext)) {
    return EFI_DEVICE_ERROR;
  }
  UINT64 Offset = 0;
  UINT64 BodyOffset = 0;
  while (Offset < PayloadRegionBytes) {
    UINT64 Remaining = PayloadRegionBytes - Offset;
    UINTN Bytes = Remaining > MT6991_UFS_DMA_MAX_TRANSFER ?
      MT6991_UFS_DMA_MAX_TRANSFER : (UINTN)Remaining;
    Status = ReadBounded (Layout, Read, Context, Buffer, SlotOffsetBlocks,
      2U * BlockSize + Offset, Bytes, PartitionEnd);
    if (Status != EFI_SUCCESS) { *ReadFailure = TRUE; return Status; }
    if (Offset == 0 && ValidateXnu && !ValidXnuBodyHeader (Buffer, BodyBytes)) {
      return EFI_COMPROMISED_DATA;
    }
    UINTN HashBytes = 0;
    if (BodyOffset < BodyBytes) {
      UINT64 HashRemaining = BodyBytes - BodyOffset;
      HashBytes = HashRemaining > Bytes ? Bytes : (UINTN)HashRemaining;
      if (HashBytes != 0 && !Sha256Update (HashContext, Buffer, HashBytes)) {
        return EFI_DEVICE_ERROR;
      }
      BodyOffset += HashBytes;
    }
    if (HashBytes < Bytes && !ZeroBytes ((CONST UINT8 *)Buffer + HashBytes, Bytes - HashBytes)) {
      return EFI_COMPROMISED_DATA;
    }
    Offset += Bytes;
  }
  if (BodyOffset != BodyBytes || !Sha256Final (HashContext, Digest)) { return EFI_DEVICE_ERROR; }
  return CompareMem (Digest, Record + LOG_BODY_HASH, SHA256_DIGEST_SIZE) ?
    EFI_COMPROMISED_DATA : EFI_SUCCESS;
}

STATIC EFI_STATUS ClassifySlot (CONST DALI_UFS_LAYOUT *Layout,
  CONST MT6991_UFS_SCRATCH_GRANT *Expected, DALI_UFS_LAYOUT_READ Read,
  VOID *Context, VOID *Buffer, VOID *HashContext, UINT64 SlotIndex,
  UINT64 SlotBlocks, UINT64 PartitionEnd, DALI_SLOT_INFO *Info) {
  UINT32 BlockSize = Layout->BlockSize;
  UINT64 SlotOffsetBlocks = SlotIndex * SlotBlocks;
  UINT64 SlotBytes = SlotBlocks * BlockSize;
  UINT64 ChunkBlocks = MT6991_UFS_DMA_MAX_TRANSFER / BlockSize;
  UINT32 Owner = (SlotIndex & 1) == 0 ? LOG_OWNER_UEFI : LOG_OWNER_XNU;
  UINT64 Sequence = SlotIndex / 2 + 1;
  UINT8 Records[3][4096];
  BOOLEAN Empty = TRUE;
  ZeroMem (Info, sizeof *Info);
  ZeroMem (Records, sizeof Records);
  if (SlotOffsetBlocks > Layout->BootBlocks ||
      SlotBlocks > Layout->BootBlocks - SlotOffsetBlocks ||
      SlotBytes != LOG_WINDOW_BYTES || ChunkBlocks == 0) { return EFI_SECURITY_VIOLATION; }

  for (UINT64 OffsetBlocks = 0; OffsetBlocks < SlotBlocks; OffsetBlocks += ChunkBlocks) {
    UINT64 Count = SlotBlocks - OffsetBlocks;
    if (Count > ChunkBlocks) { Count = ChunkBlocks; }
    UINTN Bytes = (UINTN)(Count * BlockSize);
    UINT64 ByteOffset = OffsetBlocks * BlockSize;
    EFI_STATUS Status = ReadBounded (Layout, Read, Context, Buffer,
      SlotOffsetBlocks, ByteOffset, Bytes, PartitionEnd);
    if (Status != EFI_SUCCESS) { return Status; }
    if (!ZeroBytes (Buffer, Bytes)) { Empty = FALSE; }
    UINT64 RecordOffsets[3] = { 0, BlockSize, SlotBytes - BlockSize };
    for (UINTN I = 0; I < ARRAY_SIZE (RecordOffsets); ++I) {
      if (RecordOffsets[I] >= ByteOffset &&
          RecordOffsets[I] + BlockSize <= ByteOffset + Bytes) {
        CopyMem (Records[I], (CONST UINT8 *)Buffer + RecordOffsets[I] - ByteOffset, BlockSize);
      }
    }
  }
  if (Empty) { Info->State = DaliUfsLogSlotEmpty; return EFI_SUCCESS; }
  Info->State = DaliUfsLogSlotUnknown;

  UINT32 AttemptSchema = 0;
  EFI_STATUS AttemptStatus = VerifyRecordBlock (Records[0], BlockSize, 1, &AttemptSchema);
  if (AttemptStatus == EFI_DEVICE_ERROR) { return AttemptStatus; }
  BOOLEAN ValidV2Attempt = FALSE;
  DALI_SLOT_INFO AttemptInfo;
  ZeroMem (&AttemptInfo, sizeof AttemptInfo);
  if (AttemptStatus == EFI_SUCCESS && AttemptSchema == 2 &&
      ValidateRecordFields (Records[0], BlockSize, SlotBlocks, Owner, Sequence,
        Expected, &AttemptInfo) == EFI_SUCCESS) {
    ValidV2Attempt = TRUE;
  }

  UINT32 Schemas[3] = { 0, 0, 0 };
  EFI_STATUS RecordStatus[3];
  for (UINTN I = 0; I < ARRAY_SIZE (Records); ++I) {
    RecordStatus[I] = VerifyRecordBlock (Records[I], BlockSize, (UINT32)I + 1, &Schemas[I]);
    if (RecordStatus[I] == EFI_DEVICE_ERROR) { return RecordStatus[I]; }
  }
  BOOLEAN CommitMatchesAttempt = RecordStatus[0] == EFI_SUCCESS &&
    RecordStatus[2] == EFI_SUCCESS && Schemas[0] == 2 && Schemas[2] == 2 &&
    SameRecordContent (Records[0], Records[2], BlockSize);
  if (ValidV2Attempt && !CommitMatchesAttempt) {
    *Info = AttemptInfo;
    Info->State = DaliUfsLogSlotInterruptedV2;
    return EFI_SUCCESS;
  }
  if (RecordStatus[0] != EFI_SUCCESS || RecordStatus[1] != EFI_SUCCESS ||
      RecordStatus[2] != EFI_SUCCESS || Schemas[0] != Schemas[1] ||
      Schemas[1] != Schemas[2] ||
      !SameRecordExceptKindAndDigest (Records[0], Records[1], Records[2], BlockSize)) {
    return EFI_SUCCESS;
  }
  EFI_STATUS Status = ValidateRecordFields (Records[1], BlockSize,
    SlotBlocks, Owner, Sequence, Expected, Info);
  if (Status != EFI_SUCCESS) { return EFI_SUCCESS; }
  UINT8 BodyDigest[SHA256_DIGEST_SIZE];
  BOOLEAN ReadFailure = FALSE;
  Status = VerifyBody (Layout, Read, Context, Buffer, HashContext,
    SlotOffsetBlocks, PartitionEnd, Records[1], BodyDigest, &ReadFailure);
  if (ReadFailure || Status == EFI_DEVICE_ERROR) { return Status; }
  if (Status != EFI_SUCCESS) { return EFI_SUCCESS; }
  Info->State = Schemas[1] == 1 ? DaliUfsLogSlotValidV1 : DaliUfsLogSlotValidV2;
  return EFI_SUCCESS;
}

STATIC BOOLEAN SameV2Pair (CONST DALI_SLOT_INFO *Uefi, CONST DALI_SLOT_INFO *Xnu) {
  return Uefi->Generation == Xnu->Generation &&
    CompareMem (Uefi->Token, Xnu->Token, sizeof Uefi->Token) == 0 &&
    CompareMem (Uefi->DeviceBinding, Xnu->DeviceBinding, sizeof Uefi->DeviceBinding) == 0 &&
    CompareMem (Uefi->CandidateSha256, Xnu->CandidateSha256, sizeof Uefi->CandidateSha256) == 0 &&
    CompareMem (Uefi->SourceSha256, Xnu->SourceSha256, sizeof Uefi->SourceSha256) == 0;
}

STATIC EFI_STATUS ComputeNextGeneration (CONST DALI_SLOT_INFO Slots[8],
  DALI_UFS_LOG_SLOT_SCAN_RESULT *Result) {
  UINT64 Generations[4];
  UINTN Count = 0;
  Result->HasCommittedV2 = FALSE;
  Result->MaxGeneration = 0;
  Result->NextGeneration = 0;
  Result->NextGenerationValid = FALSE;
  Result->HasInconsistentPair = FALSE;
  for (UINTN Pair = 0; Pair < 4; ++Pair) {
    CONST DALI_SLOT_INFO *Uefi = &Slots[Pair * 2];
    CONST DALI_SLOT_INFO *Xnu = &Slots[Pair * 2 + 1];
    BOOLEAN Consistent = FALSE;
    if (Uefi->State == DaliUfsLogSlotEmpty && Xnu->State == DaliUfsLogSlotEmpty) {
      Consistent = TRUE;
    } else if (Uefi->State == DaliUfsLogSlotValidV2 &&
               (Xnu->State == DaliUfsLogSlotEmpty ||
                (Xnu->State == DaliUfsLogSlotValidV2 && SameV2Pair (Uefi, Xnu)))) {
      Consistent = TRUE;
    }
    Result->PairConsistent[Pair] = Consistent;
    if (Xnu->State == DaliUfsLogSlotValidV2 &&
        (Uefi->State != DaliUfsLogSlotValidV2 || !SameV2Pair (Uefi, Xnu))) {
      Result->HasInconsistentPair = TRUE;
    } else if (Uefi->State == DaliUfsLogSlotValidV2 &&
               Xnu->State != DaliUfsLogSlotEmpty &&
               (Xnu->State != DaliUfsLogSlotValidV2 || !SameV2Pair (Uefi, Xnu))) {
      Result->HasInconsistentPair = TRUE;
    }
    if (Uefi->State != DaliUfsLogSlotValidV2) { continue; }
    for (UINTN I = 0; I < Count; ++I) {
      if (Generations[I] == Uefi->Generation) { return EFI_COMPROMISED_DATA; }
    }
    Generations[Count++] = Uefi->Generation;
    Result->HasCommittedV2 = TRUE;
    if (Uefi->Generation > Result->MaxGeneration) { Result->MaxGeneration = Uefi->Generation; }
  }
  if (Result->MaxGeneration == MAX_UINT64) { return EFI_OUT_OF_RESOURCES; }
  Result->NextGeneration = Result->HasCommittedV2 ? Result->MaxGeneration + 1 : 1;
  Result->NextGenerationValid = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS DaliUfsClassifyUserdataLogSlots (CONST DALI_UFS_LAYOUT *Layout,
  CONST MT6991_UFS_SCRATCH_GRANT *Expected, UINT32 ParentAlign, UINT32 ScsiAlign,
  DALI_UFS_LAYOUT_READ Read, VOID *Context, DALI_UFS_LOG_SLOT_SCAN_RESULT *Result) {
  if (Result == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Result, sizeof *Result);
  for (UINTN Slot = 0; Slot < DALI_UFS_LOG_SLOT_COUNT; ++Slot) {
    Result->Slots[Slot] = DaliUfsLogSlotUnknown;
  }
  if (Layout == NULL || Expected == NULL || Read == NULL || !Layout->Verified ||
      !Mt6991UfsScratchGrantValid (Expected) ||
      Layout->Target != MT6991_UFS_TARGET_USERDATA_LOGS ||
      Expected->Target != MT6991_UFS_TARGET_USERDATA_LOGS ||
      Layout->Lun != 2 || Expected->Lun != Layout->Lun ||
      Layout->BlockSize != 4096 || Expected->BlockSize != Layout->BlockSize ||
      Layout->CapacityBlocks != 64577536ULL || Expected->CapacityBlocks != Layout->CapacityBlocks ||
      Layout->BootStartLba != 64565240ULL || Expected->BootStartLba != Layout->BootStartLba ||
      Layout->BootBlocks != 8192ULL || Expected->BootBlocks != Layout->BootBlocks ||
      Layout->ScratchBlocks != MT6991_UFS_DATA_LOG_SLOT_BYTES / Layout->BlockSize ||
      Expected->ScratchBlocks != Layout->ScratchBlocks ||
      ZeroBytes (Expected->DeviceBinding, sizeof Expected->DeviceBinding) ||
      ZeroBytes (Expected->CandidateSha256, sizeof Expected->CandidateSha256) ||
      ZeroBytes (Expected->SourceSha256, sizeof Expected->SourceSha256) ||
      CompareMem (Layout->PartitionGuid, mPartitionGuid, sizeof mPartitionGuid) ||
      CompareMem (Expected->PartitionGuid, Layout->PartitionGuid, sizeof mPartitionGuid)) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT64 PartitionEnd = Layout->BootStartLba + Layout->BootBlocks;
  if (PartitionEnd < Layout->BootStartLba || PartitionEnd > Layout->CapacityBlocks) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT32 Align = ParentAlign > ScsiAlign ? ParentAlign : ScsiAlign;
  if (Align > EFI_PAGE_SIZE || (Align != 0 && (Align & (Align - 1)) != 0)) {
    return EFI_UNSUPPORTED;
  }
  if (Align < EFI_PAGE_SIZE) { Align = EFI_PAGE_SIZE; }
  UINTN ContextBytes = Sha256GetContextSize ();
  if (ContextBytes == 0 || ContextBytes > 4096) { return EFI_UNSUPPORTED; }
  VOID *Buffer = AllocateAlignedPages (EFI_SIZE_TO_PAGES (MT6991_UFS_DMA_MAX_TRANSFER), Align);
  VOID *HashContext = AllocatePool (ContextBytes);
  if (Buffer == NULL || HashContext == NULL) {
    if (Buffer != NULL) { FreeAlignedPages (Buffer, EFI_SIZE_TO_PAGES (MT6991_UFS_DMA_MAX_TRANSFER)); }
    if (HashContext != NULL) { FreePool (HashContext); }
    return EFI_OUT_OF_RESOURCES;
  }

  DALI_SLOT_INFO Slots[DALI_UFS_LOG_SLOT_COUNT];
  ZeroMem (Slots, sizeof Slots);
  UINT64 SlotBlocks = Layout->ScratchBlocks;
  EFI_STATUS Status = EFI_SUCCESS;
  for (UINT64 Slot = 0; Slot < DALI_UFS_LOG_SLOT_COUNT && Status == EFI_SUCCESS; ++Slot) {
    Status = ClassifySlot (Layout, Expected, Read, Context, Buffer, HashContext,
      Slot, SlotBlocks, PartitionEnd, &Slots[Slot]);
    Result->Slots[Slot] = Slots[Slot].State;
  }
  if (Status == EFI_SUCCESS) { Status = ComputeNextGeneration (Slots, Result); }
  ZeroMem (HashContext, ContextBytes);
  FreePool (HashContext);
  ZeroMem (Buffer, MT6991_UFS_DMA_MAX_TRANSFER);
  FreeAlignedPages (Buffer, EFI_SIZE_TO_PAGES (MT6991_UFS_DMA_MAX_TRANSFER));
  return Status;
}
