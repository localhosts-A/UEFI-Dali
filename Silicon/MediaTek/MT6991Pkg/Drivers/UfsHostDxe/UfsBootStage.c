// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/DaliBootStage.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include "UfsHostInternal.h"

#define DALI_BOOT_STAGE_BLOCK_SIZE       4096U
#define DALI_BOOT_STAGE_LOGICAL_SIZE     512U
#define DALI_BOOT_STAGE_MAX_READ         (1024U * 1024U)
#define DALI_BOOT_STAGE_MAX_BYTES        (512ULL * 1024ULL * 1024ULL)
#define DALI_BOOT_STAGE_LUN              2U
#define DALI_BOOT_STAGE_FIRST_LBA        7023103ULL
#define DALI_BOOT_STAGE_LAST_LBA         15403518ULL
#define DALI_BOOT_STAGE_MIB              (1024ULL * 1024ULL)

STATIC EFI_BLOCK_IO_PROTOCOL *mRootBlockIo;
STATIC EFI_BLOCK_IO_MEDIA mRootMedia;
STATIC DALI_BOOT_STAGE_DESCRIPTOR mDescriptorStorage;
STATIC DALI_BOOT_STAGE_DESCRIPTOR *mDescriptor;
STATIC EFI_BLOCK_IO_MEDIA mBootMedia;
STATIC VOID *mIoBuffer;
STATIC EFI_HANDLE mBootHandle;
STATIC BOOLEAN mBootPublished;
STATIC BOOLEAN mBootClosed;
STATIC BOOLEAN mReadBusy;
STATIC EFI_BLOCK_IO_PROTOCOL mBootIo;

STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mBootPath = {
  { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x83cf30a1, 0x7839, 0x48c2, { 0x9e, 0x59, 0x14, 0xb5, 0x6f, 0x6b, 0x1d, 0x73 } } },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC BOOLEAN IsPowerOfTwo (UINT32 Value) {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

STATIC EFI_STATUS PathIdentity (EFI_DEVICE_PATH_PROTOCOL *Path, UINT8 *Lun,
  EFI_LBA *Start, EFI_LBA *End) {
  UINTN Size;
  BOOLEAN SawUfs = FALSE, SawPartition = FALSE;
  if (Path == NULL || Lun == NULL || Start == NULL || End == NULL) { return EFI_INVALID_PARAMETER; }
  Size = GetDevicePathSize (Path);
  if (Size < sizeof (EFI_DEVICE_PATH_PROTOCOL) || Size > 2048) { return EFI_COMPROMISED_DATA; }
  for (UINTN At = 0; At + sizeof (EFI_DEVICE_PATH_PROTOCOL) <= Size;) {
    EFI_DEVICE_PATH_PROTOCOL *Node = (VOID *)((UINT8 *)Path + At);
    UINTN Bytes = DevicePathNodeLength (Node);
    if (Bytes < sizeof (*Node) || Bytes > Size - At) { return EFI_COMPROMISED_DATA; }
    if (IsDevicePathEnd (Node)) {
      return SawUfs && SawPartition ? EFI_SUCCESS : EFI_NOT_FOUND;
    }
    if (DevicePathType (Node) == MESSAGING_DEVICE_PATH && DevicePathSubType (Node) == MSG_UFS_DP) {
      if (SawUfs || Bytes != sizeof (UFS_DEVICE_PATH)) { return EFI_COMPROMISED_DATA; }
      *Lun = ((UFS_DEVICE_PATH *)Node)->Lun;
      SawUfs = TRUE;
    } else if (DevicePathType (Node) == MEDIA_DEVICE_PATH && DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP) {
      if (!SawUfs || SawPartition || Bytes != sizeof (HARDDRIVE_DEVICE_PATH)) { return EFI_COMPROMISED_DATA; }
      HARDDRIVE_DEVICE_PATH *HardDrive = (VOID *)Node;
      if (HardDrive->SignatureType != SIGNATURE_TYPE_GUID || HardDrive->PartitionSize == 0 ||
          HardDrive->PartitionStart > MAX_UINT64 - HardDrive->PartitionSize) { return EFI_COMPROMISED_DATA; }
      *Start = HardDrive->PartitionStart;
      *End = HardDrive->PartitionStart + HardDrive->PartitionSize - 1;
      SawPartition = TRUE;
    }
    At += Bytes;
  }
  return EFI_COMPROMISED_DATA;
}

STATIC EFI_STATUS ValidateDescriptor (DALI_BOOT_STAGE_DESCRIPTOR *Descriptor, UINTN Bytes) {
  UINT8 Digest[SHA256_DIGEST_SIZE];
  UINT64 Capacity = (DALI_BOOT_STAGE_LAST_LBA - DALI_BOOT_STAGE_FIRST_LBA + 1) * DALI_BOOT_STAGE_BLOCK_SIZE;
  if (Descriptor == NULL || Bytes != sizeof (*Descriptor) ||
      CompareMem (Descriptor->Magic, DALI_BOOT_STAGE_DESCRIPTOR_MAGIC, 8) != 0 ||
      Descriptor->Version != DALI_BOOT_STAGE_DESCRIPTOR_VERSION ||
      Descriptor->DescriptorBytes != sizeof (*Descriptor) ||
      Descriptor->PartitionBytes != Capacity || Descriptor->FirstLba != DALI_BOOT_STAGE_FIRST_LBA ||
      Descriptor->LastLba != DALI_BOOT_STAGE_LAST_LBA || Descriptor->Lun != DALI_BOOT_STAGE_LUN ||
      Descriptor->BlockSize != DALI_BOOT_STAGE_BLOCK_SIZE || Descriptor->RootImageBytes < DALI_BOOT_STAGE_MIB ||
      Descriptor->BootOffsetBytes < Descriptor->RootImageBytes ||
      (Descriptor->BootOffsetBytes & (DALI_BOOT_STAGE_MIB - 1)) != 0 ||
      Descriptor->BootBytes < DALI_BOOT_STAGE_BLOCK_SIZE || Descriptor->BootBytes > DALI_BOOT_STAGE_MAX_BYTES ||
      (Descriptor->BootBytes % DALI_BOOT_STAGE_BLOCK_SIZE) != 0 ||
      Descriptor->BootOffsetBytes > Capacity || Descriptor->BootBytes > Capacity - Descriptor->BootOffsetBytes) {
    return EFI_COMPROMISED_DATA;
  }
  if (!Sha256HashAll (Descriptor, sizeof (*Descriptor) - sizeof (Descriptor->DescriptorSha256), Digest) ||
      CompareMem (Digest, Descriptor->DescriptorSha256, sizeof Digest) != 0) { return EFI_SECURITY_VIOLATION; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS LocateRootPartition (EFI_HANDLE Controller, EFI_BLOCK_IO_PROTOCOL **BlockIo,
  EFI_BLOCK_IO_MEDIA *Snapshot) {
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Matches = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Index = 0; Index < Count; ++Index) {
    EFI_PARTITION_INFO_PROTOCOL *Info = NULL;
    EFI_BLOCK_IO_PROTOCOL *Io = NULL;
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    UINT8 Lun = MAX_UINT8;
    EFI_LBA Start = 0, End = 0;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || Info == NULL || Info->Type != PARTITION_TYPE_GPT ||
        StrCmp (Info->Info.Gpt.PartitionName, L"unixnu_data") != 0) { continue; }
    ++Matches;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Status) || Io == NULL || Io->Media == NULL) { Status = EFI_DEVICE_ERROR; break; }
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (EFI_ERROR (Status)) { break; }
    Status = PathIdentity (Path, &Lun, &Start, &End);
    if (EFI_ERROR (Status) || Lun != DALI_BOOT_STAGE_LUN ||
        Info->Info.Gpt.StartingLBA != DALI_BOOT_STAGE_FIRST_LBA ||
        Info->Info.Gpt.EndingLBA != DALI_BOOT_STAGE_LAST_LBA ||
        Start != DALI_BOOT_STAGE_FIRST_LBA || End != DALI_BOOT_STAGE_LAST_LBA ||
        Io->Media->BlockSize != DALI_BOOT_STAGE_BLOCK_SIZE || !Io->Media->ReadOnly ||
        !Io->Media->MediaPresent || Io->Media->LastBlock != DALI_BOOT_STAGE_LAST_LBA - DALI_BOOT_STAGE_FIRST_LBA) {
      Status = EFI_SECURITY_VIOLATION;
      break;
    }
    (VOID)Controller;
    *BlockIo = Io;
    CopyMem (Snapshot, Io->Media, sizeof (*Snapshot));
    Status = EFI_SUCCESS;
  }
  if (Handles != NULL) { FreePool (Handles); }
  if (EFI_ERROR (Status)) { return Status; }
  return Matches == 1 ? EFI_SUCCESS : (Matches == 0 ? EFI_NOT_FOUND : EFI_SECURITY_VIOLATION);
}

STATIC EFI_STATUS ValidateRootManifest (VOID) {
  UINT8 Digest[SHA256_DIGEST_SIZE], ManifestDigest[SHA256_DIGEST_SIZE];
  UINT8 *Bytes = mIoBuffer;
  EFI_STATUS Status = mRootBlockIo->ReadBlocks (mRootBlockIo, mRootMedia.MediaId, 0,
    DALI_BOOT_STAGE_BLOCK_SIZE, Bytes);
  if (EFI_ERROR (Status)) { return Status; }
  if (CompareMem (Bytes, "SHNGROOT", 8) != 0 || ReadUnaligned32 ((UINT32 *)(Bytes + 8)) != 1 ||
      ReadUnaligned32 ((UINT32 *)(Bytes + 12)) != 2 ||
      ReadUnaligned64 ((UINT64 *)(Bytes + 16)) != mDescriptor->RootImageBytes ||
      CompareMem (Bytes + 24, "OS", 2) != 0 || Bytes[26] != 0 ||
      CompareMem (Bytes + 24 + 128, "SystemOS", 8) != 0 || Bytes[24 + 128 + 8] != 0 ||
      ReadUnaligned32 ((UINT32 *)(Bytes + 24 + 48)) != 512 ||
      ReadUnaligned32 ((UINT32 *)(Bytes + 24 + 52)) != 1 ||
      ReadUnaligned32 ((UINT32 *)(Bytes + 24 + 128 + 48)) != 512 ||
      ReadUnaligned32 ((UINT32 *)(Bytes + 24 + 128 + 52)) != 1 ||
      !Sha256HashAll (Bytes, 4064, Digest) || CompareMem (Digest, Bytes + 4064, sizeof Digest) != 0 ||
      !Sha256HashAll (Bytes, DALI_BOOT_STAGE_BLOCK_SIZE, ManifestDigest) ||
      CompareMem (ManifestDigest, mDescriptor->RootManifestSha256, sizeof ManifestDigest) != 0) {
    return EFI_SECURITY_VIOLATION;
  }
  for (UINTN Index = 24 + 2 * 128; Index < 4064; ++Index) {
    if (Bytes[Index] != 0) { return EFI_COMPROMISED_DATA; }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS VerifyBootHash (VOID) {
  UINTN ContextBytes = Sha256GetContextSize ();
  VOID *Context = NULL;
  UINT8 Digest[SHA256_DIGEST_SIZE];
  UINT64 Remaining = mDescriptor->BootBytes;
  EFI_LBA Lba = mDescriptor->BootOffsetBytes / DALI_BOOT_STAGE_BLOCK_SIZE;
  EFI_STATUS Status = EFI_OUT_OF_RESOURCES;
  if (ContextBytes == 0) { return EFI_UNSUPPORTED; }
  Context = AllocatePool (ContextBytes);
  if (Context == NULL) { return EFI_OUT_OF_RESOURCES; }
  if (!Sha256Init (Context)) { Status = EFI_DEVICE_ERROR; goto Done; }
  while (Remaining != 0) {
    UINTN Amount = (UINTN)MIN (Remaining, DALI_BOOT_STAGE_MAX_READ);
    if (Amount == 0 || (Amount % DALI_BOOT_STAGE_BLOCK_SIZE) != 0) { Status = EFI_COMPROMISED_DATA; goto Done; }
    Status = mRootBlockIo->ReadBlocks (mRootBlockIo, mRootMedia.MediaId, Lba, Amount, mIoBuffer);
    if (EFI_ERROR (Status)) { goto Done; }
    if (!Sha256Update (Context, mIoBuffer, Amount)) { Status = EFI_DEVICE_ERROR; goto Done; }
    Lba += Amount / DALI_BOOT_STAGE_BLOCK_SIZE;
    Remaining -= Amount;
  }
  if (!Sha256Final (Context, Digest)) { Status = EFI_DEVICE_ERROR; goto Done; }
  Status = CompareMem (Digest, mDescriptor->BootSha256, sizeof Digest) == 0 ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
Done:
  if (Context != NULL) { FreePool (Context); }
  return Status;
}

STATIC EFI_STATUS EFIAPI BootReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended) {
  (VOID)Extended;
  return This == &mBootIo ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS EFIAPI BootRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  UINTN Done = 0;
  EFI_STATUS Status = EFI_SUCCESS;
  if (This != &mBootIo || Buffer == NULL) { return EFI_INVALID_PARAMETER; }
  if (MediaId != mBootMedia.MediaId) { return EFI_MEDIA_CHANGED; }
  if (Size == 0) { return EFI_SUCCESS; }
  if ((Size % DALI_BOOT_STAGE_LOGICAL_SIZE) != 0 || Lba > mBootMedia.LastBlock ||
      Size / DALI_BOOT_STAGE_LOGICAL_SIZE - 1 > mBootMedia.LastBlock - Lba) { return EFI_BAD_BUFFER_SIZE; }
  if (mBootClosed || mRootBlockIo == NULL) { return EFI_DEVICE_ERROR; }
  if (mReadBusy) { return EFI_NOT_READY; }
  mReadBusy = TRUE;
  while (Done < Size) {
    UINT64 LogicalOffset = Lba * DALI_BOOT_STAGE_LOGICAL_SIZE + Done;
    UINT64 PhysicalOffset = mDescriptor->BootOffsetBytes + LogicalOffset;
    UINT64 PhysicalLba = PhysicalOffset / DALI_BOOT_STAGE_BLOCK_SIZE;
    UINTN InBlock = (UINTN)(PhysicalOffset % DALI_BOOT_STAGE_BLOCK_SIZE);
    UINTN Amount = MIN (Size - Done, DALI_BOOT_STAGE_MAX_READ - InBlock);
    UINTN PhysicalBytes;
    Amount &= ~(DALI_BOOT_STAGE_LOGICAL_SIZE - 1U);
    if (Amount == 0) { Status = EFI_BAD_BUFFER_SIZE; break; }
    PhysicalBytes = (InBlock + Amount + DALI_BOOT_STAGE_BLOCK_SIZE - 1) & ~(DALI_BOOT_STAGE_BLOCK_SIZE - 1U);
    if (PhysicalBytes > DALI_BOOT_STAGE_MAX_READ) { Status = EFI_BAD_BUFFER_SIZE; break; }
    Status = mRootBlockIo->ReadBlocks (mRootBlockIo, mRootMedia.MediaId, PhysicalLba, PhysicalBytes, mIoBuffer);
    if (EFI_ERROR (Status)) { break; }
    CopyMem ((UINT8 *)Buffer + Done, (UINT8 *)mIoBuffer + InBlock, Amount);
    Done += Amount;
  }
  mReadBusy = FALSE;
  return Status;
}

STATIC EFI_STATUS EFIAPI BootWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  (VOID)Buffer;
  if (This != &mBootIo) { return EFI_INVALID_PARAMETER; }
  if (MediaId != mBootMedia.MediaId) { return EFI_MEDIA_CHANGED; }
  if (Size != 0 && (Size % DALI_BOOT_STAGE_LOGICAL_SIZE != 0 || Lba > mBootMedia.LastBlock ||
      Size / DALI_BOOT_STAGE_LOGICAL_SIZE - 1 > mBootMedia.LastBlock - Lba)) { return EFI_INVALID_PARAMETER; }
  return Size == 0 ? EFI_SUCCESS : EFI_WRITE_PROTECTED;
}

STATIC EFI_STATUS EFIAPI BootFlush (EFI_BLOCK_IO_PROTOCOL *This) {
  return This == &mBootIo ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

STATIC EFI_BLOCK_IO_PROTOCOL mBootIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION, &mBootMedia, BootReset, BootRead, BootWrite, BootFlush
};

EFI_STATUS DaliUfsBootStagePublish (EFI_HANDLE Controller) {
  VOID *Raw = NULL;
  UINTN RawBytes = 0;
  EFI_STATUS Status;
  UINT32 Alignment;
  if (mBootPublished || Controller == NULL) { return EFI_ALREADY_STARTED; }
  Status = GetSectionFromAnyFv (&gDaliBootStageFileGuid, EFI_SECTION_RAW, 0, &Raw, &RawBytes);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ValidateDescriptor (Raw, RawBytes);
  if (EFI_ERROR (Status)) { goto Done; }
  CopyMem (&mDescriptorStorage, Raw, sizeof mDescriptorStorage);
  mDescriptor = &mDescriptorStorage;
  Status = LocateRootPartition (Controller, &mRootBlockIo, &mRootMedia);
  if (EFI_ERROR (Status)) { goto Done; }
  if ((mRootMedia.LastBlock + 1) * mRootMedia.BlockSize != mDescriptor->PartitionBytes) {
    Status = EFI_SECURITY_VIOLATION;
    goto Done;
  }
  Alignment = MAX (mRootMedia.IoAlign, EFI_PAGE_SIZE);
  if (Alignment > 65536 || !IsPowerOfTwo (Alignment)) { Status = EFI_UNSUPPORTED; goto Done; }
  mIoBuffer = AllocateAlignedPages (EFI_SIZE_TO_PAGES (DALI_BOOT_STAGE_MAX_READ), Alignment);
  if (mIoBuffer == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = ValidateRootManifest ();
  if (EFI_ERROR (Status)) { goto Done; }
  Status = VerifyBootHash ();
  if (EFI_ERROR (Status)) { goto Done; }
  ZeroMem (&mBootMedia, sizeof mBootMedia);
  mBootMedia.MediaId = 1;
  mBootMedia.MediaPresent = TRUE;
  mBootMedia.LogicalPartition = TRUE;
  mBootMedia.ReadOnly = TRUE;
  mBootMedia.BlockSize = DALI_BOOT_STAGE_LOGICAL_SIZE;
  mBootMedia.IoAlign = 1;
  mBootMedia.LastBlock = mDescriptor->BootBytes / DALI_BOOT_STAGE_LOGICAL_SIZE - 1;
  Status = gBS->InstallMultipleProtocolInterfaces (&mBootHandle,
    &gEfiDevicePathProtocolGuid, &mBootPath,
    &gEfiBlockIoProtocolGuid, &mBootIo, NULL);
  if (!EFI_ERROR (Status)) { mBootPublished = TRUE; }
  DEBUG ((DEBUG_WARN, "DALI-UFS-BOOT: publish status=%r bytes=%Lu sha256-verified=1 readonly=1\n",
    Status, mDescriptor->BootBytes));
Done:
  if (EFI_ERROR (Status)) {
    mRootBlockIo = NULL;
    if (mIoBuffer != NULL) { FreePages (mIoBuffer, EFI_SIZE_TO_PAGES (DALI_BOOT_STAGE_MAX_READ)); mIoBuffer = NULL; }
    mDescriptor = NULL;
  }
  if (Raw != NULL) { FreePool (Raw); }
  return Status;
}

EFI_STATUS DaliUfsBootStageCloseAdmission (VOID) {
  if (!mBootPublished) { return EFI_SUCCESS; }
  if (mReadBusy) { return EFI_NOT_READY; }
  mBootClosed = TRUE;
  return EFI_SUCCESS;
}
