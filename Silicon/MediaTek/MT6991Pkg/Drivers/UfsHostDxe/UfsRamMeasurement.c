// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Guid/FileInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include "UfsProvenance.h"

STATIC CONST struct { VENDOR_DEVICE_PATH Vendor; EFI_DEVICE_PATH_PROTOCOL End; } RamPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof (VENDOR_DEVICE_PATH), 0}},
   {0x3B2059CE, 0xCA2B, 0x4B45, {0x91, 0xCB, 0xB2, 0x4F, 0x4E, 0xF3, 0x52, 0x7A}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}
};
STATIC EFI_STATUS Exact (EFI_STATUS Status) { return Status == EFI_SUCCESS ? Status : (EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR); }
STATIC BOOLEAN Same (EFI_BLOCK_IO_PROTOCOL *Block, CONST EFI_BLOCK_IO_MEDIA *Before) {
  return Block->Media != NULL && Block->Media->MediaId == Before->MediaId &&
    Block->Media->ReadOnly && Block->Media->MediaPresent && Block->Media->LogicalPartition &&
    Block->Media->BlockSize == Before->BlockSize && Block->Media->LastBlock == Before->LastBlock;
}
STATIC EFI_STATUS FileSize (EFI_FILE_PROTOCOL *File, UINT64 *Bytes) {
  UINT64 Storage[128]; UINTN Size = sizeof Storage;
  EFI_STATUS Status = Exact (File->GetInfo (File, &gEfiFileInfoGuid, &Size, Storage));
  if (Status != EFI_SUCCESS) { return Status; }
  EFI_FILE_INFO *Info = (VOID *)Storage;
  if (Size < SIZE_OF_EFI_FILE_INFO || Size > sizeof Storage || Info->Size != Size ||
      (Info->Attribute & EFI_FILE_DIRECTORY) || Info->FileSize < 32 ||
      Info->FileSize > DALI_UFS_KERNEL_MAX_BYTES) { return EFI_COMPROMISED_DATA; }
  *Bytes = Info->FileSize; return EFI_SUCCESS;
}
EFI_STATUS DaliUfsMeasureRamPayload (CONST VOID *CompiledSource, UINTN SourceBytes,
  DALI_UFS_PROVENANCE *Observed) {
  if (Observed == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Observed, sizeof *Observed);
  if (CompiledSource == NULL || SourceBytes != DALI_UFS_SOURCE_BYTES) { return EFI_SECURITY_VIOLATION; }
  EFI_HANDLE *Handles = NULL, Selected = NULL;
  UINTN Count = 0, Matches = 0;
  EFI_STATUS Status = Exact (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles));
  if (Status != EFI_SUCCESS) { return Status; }
  if (Handles == NULL || Count == 0 || Count > 1024) {
    if (Handles != NULL) { FreePool (Handles); }
    return EFI_COMPROMISED_DATA;
  }
  for (UINTN I = 0; I < Count; ++I) {
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    if (gBS->HandleProtocol (Handles[I], &gEfiDevicePathProtocolGuid, (VOID **)&Path) != EFI_SUCCESS || Path == NULL) { continue; }
    // Walk the one expected vendor node only after validating its length. Avoid
    // reading sizeof(RamPath) from an arbitrary shorter device path allocation.
    if (Path->Type != HARDWARE_DEVICE_PATH || Path->SubType != HW_VENDOR_DP ||
        Path->Length[0] != sizeof (VENDOR_DEVICE_PATH) || Path->Length[1] != 0) { continue; }
    if (CompareMem (Path, &RamPath.Vendor, sizeof RamPath.Vendor)) { continue; }
    EFI_DEVICE_PATH_PROTOCOL *End = (VOID *)((UINT8 *)Path + sizeof RamPath.Vendor);
    if (CompareMem (End, &RamPath.End, sizeof RamPath.End)) { continue; }
    ++Matches; Selected = Handles[I];
  }
  FreePool (Handles);
  if (Matches != 1) { return Matches == 0 ? EFI_NOT_FOUND : EFI_SECURITY_VIOLATION; }
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL; EFI_BLOCK_IO_PROTOCOL *Block = NULL;
  Status = Exact (gBS->HandleProtocol (Selected, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs));
  if (Status == EFI_SUCCESS) { Status = Exact (gBS->HandleProtocol (Selected, &gEfiBlockIoProtocolGuid, (VOID **)&Block)); }
  if (Status != EFI_SUCCESS) { return Status; }
  if (Fs == NULL || Block == NULL || Block->Media == NULL) { return EFI_COMPROMISED_DATA; }
  EFI_BLOCK_IO_MEDIA Before = *Block->Media;
  if (!Same (Block, &Before) || Before.BlockSize != 512 || Before.LastBlock < 1 ||
      Before.LastBlock >= 0x4000000U / 512U) { return EFI_SECURITY_VIOLATION; }
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  VOID *Data = NULL;
  Status = Exact (Fs->OpenVolume (Fs, &Root));
  if (Status != EFI_SUCCESS) { return Status; }
  if (Root == NULL) { return EFI_COMPROMISED_DATA; }
  Status = Exact (Root->Open (Root, &File, L"\\EFI\\UniXNU\\Payloads\\kernel.macho", EFI_FILE_MODE_READ, 0));
  if (Status == EFI_SUCCESS && File == NULL) { Status = EFI_COMPROMISED_DATA; }
  UINT64 Bytes = 0, After = 0;
  if (Status == EFI_SUCCESS) { Status = FileSize (File, &Bytes); }
  if (Status == EFI_SUCCESS && Bytes > (Before.LastBlock + 1) * 512U) { Status = EFI_COMPROMISED_DATA; }
  if (Status == EFI_SUCCESS) {
    Data = AllocatePool ((UINTN)Bytes);
    if (Data == NULL) { Status = EFI_OUT_OF_RESOURCES; }
  }
  if (Status == EFI_SUCCESS) { Status = Exact (File->SetPosition (File, 0)); }
  if (Status == EFI_SUCCESS) {
    UINTN Actual = (UINTN)Bytes;
    Status = Exact (File->Read (File, &Actual, Data));
    if (Status == EFI_SUCCESS && Actual != Bytes) { Status = EFI_DEVICE_ERROR; }
  }
  if (Status == EFI_SUCCESS) {
    UINT8 Tail; UINTN One = 1;
    Status = Exact (File->Read (File, &One, &Tail));
    if (Status == EFI_SUCCESS && One != 0) { Status = EFI_MEDIA_CHANGED; }
  }
  if (Status == EFI_SUCCESS) { Status = FileSize (File, &After); }
  if (Status == EFI_SUCCESS && (After != Bytes || !Same (Block, &Before))) { Status = EFI_MEDIA_CHANGED; }
  // Close all handles before making the resulting observation available.
  if (File != NULL) { EFI_STATUS Closed = Exact (File->Close (File)); if (Status == EFI_SUCCESS) { Status = Closed; } }
  EFI_STATUS Closed = Exact (Root->Close (Root)); if (Status == EFI_SUCCESS) { Status = Closed; }
  if (Status == EFI_SUCCESS && !Same (Block, &Before)) { Status = EFI_MEDIA_CHANGED; }
  if (Status == EFI_SUCCESS) { Status = DaliUfsMeasureKernel (Data, (UINTN)Bytes, CompiledSource, SourceBytes, Observed); }
  if (Data != NULL) { FreePool (Data); }
  return Status;
}
