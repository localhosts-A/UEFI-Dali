// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only, firmware-owned FAT volume. No physical storage MMIO is involved.
#include <PiDxe.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC UINT8 *mData;
STATIC UINTN mSize;
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof (VENDOR_DEVICE_PATH), 0}},
   {0x3B2059CE, 0xCA2B, 0x4B45, {0x91, 0xCB, 0xB2, 0x4F, 0x4E, 0xF3, 0x52, 0x7A}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}
};

STATIC EFI_STATUS EFIAPI Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended) {
  return This == NULL ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

STATIC EFI_STATUS CheckRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  if (This == NULL) { return EFI_INVALID_PARAMETER; }
  if (MediaId != mMedia.MediaId) { return EFI_MEDIA_CHANGED; }
  if (Size == 0) { return EFI_SUCCESS; }
  if (Buffer == NULL) { return EFI_INVALID_PARAMETER; }
  if (Size % 512 != 0) { return EFI_BAD_BUFFER_SIZE; }
  if (Lba > mMedia.LastBlock || Size / 512 - 1 > mMedia.LastBlock - Lba) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Read (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  EFI_STATUS Status = CheckRead (This, MediaId, Lba, Size, Buffer);
  if (!EFI_ERROR (Status) && Size != 0) { CopyMem (Buffer, mData + (UINTN)Lba * 512, Size); }
  return Status;
}

STATIC EFI_STATUS EFIAPI Write (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  EFI_STATUS Status = CheckRead (This, MediaId, Lba, Size, Buffer);
  return EFI_ERROR (Status) || Size == 0 ? Status : EFI_WRITE_PROTECTED;
}

STATIC EFI_STATUS EFIAPI Flush (EFI_BLOCK_IO_PROTOCOL *This) {
  return This == NULL ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION, &mMedia, Reset, Read, Write, Flush
};

EFI_STATUS EFIAPI DaliPayloadInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  EFI_HANDLE Handle = NULL;
  UINT32 Sectors;
  Status = GetSectionFromAnyFv (&gDaliPayloadFileGuid, EFI_SECTION_RAW, 0, (VOID **)&mData, &mSize);
  if (EFI_ERROR (Status)) { return Status; }
  // Host packaging hashes the entire volume. Recheck the FAT BPB and length
  // before publishing a provider so malformed lengths never escape the buffer.
  if (mSize < 512 || mSize > 0x4000000 || mSize % 512 != 0 ||
      mData[510] != 0x55 || mData[511] != 0xAA || ReadUnaligned16 ((UINT16 *)(mData + 11)) != 512) {
    FreePool (mData); mData = NULL; return EFI_COMPROMISED_DATA;
  }
  Sectors = ReadUnaligned16 ((UINT16 *)(mData + 19));
  if (Sectors == 0) { Sectors = ReadUnaligned32 ((UINT32 *)(mData + 32)); }
  if (Sectors != mSize / 512) { FreePool (mData); mData = NULL; return EFI_COMPROMISED_DATA; }
  mMedia.MediaId = 1;
  mMedia.MediaPresent = TRUE;
  mMedia.LogicalPartition = TRUE;
  mMedia.ReadOnly = TRUE;
  mMedia.BlockSize = 512;
  mMedia.IoAlign = 1;
  mMedia.LastBlock = mSize / 512 - 1;
  Status = gBS->InstallMultipleProtocolInterfaces (&Handle,
    &gEfiDevicePathProtocolGuid, &mPath, &gEfiBlockIoProtocolGuid, &mBlockIo, NULL);
  DEBUG ((DEBUG_WARN, "DALI: RAM payload BlockIo status=%r bytes=%lu readonly=1\n", Status, mSize));
  if (EFI_ERROR (Status)) { FreePool (mData); mData = NULL; }
  return Status;
}
