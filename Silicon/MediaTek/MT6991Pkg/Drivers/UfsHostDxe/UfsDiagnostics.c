// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include "UfsHostInternal.h"

// These four values identify the previously captured Android v3 baseline's
// boot_b sample. They are checked against observed media, never used as the
// geometry of other LUNs or as an authorization to write.
#define BASELINE_LUN 2
#define BASELINE_BLOCK_SIZE 4096U
#define BASELINE_LAST_LBA 64577535ULL
#define BASELINE_BOOT_B_LBA 418560ULL

typedef struct { BOOLEAN Present; UINT64 Start, Blocks; UINT32 Number; UINT8 Signature[16]; } PARTITION_PATH;

STATIC EFI_STATUS MediaLun (EFI_DEVICE_PATH_PROTOCOL *Parent, EFI_DEVICE_PATH_PROTOCOL *Path, UINT8 *Lun, PARTITION_PATH *Partition) {
  UINTN ParentSize = GetDevicePathSize (Parent), Size = GetDevicePathSize (Path);
  if (ParentSize < sizeof (VENDOR_DEVICE_PATH) + sizeof (EFI_DEVICE_PATH_PROTOCOL) || ParentSize > 1024 || Size > 1024) { return EFI_DEVICE_ERROR; }
  UINTN Prefix = ParentSize - sizeof (EFI_DEVICE_PATH_PROTOCOL);
  if (Size < ParentSize || CompareMem (Parent, Path, Prefix)) { return EFI_NOT_FOUND; }
  BOOLEAN Found = FALSE;
  for (UINTN At = Prefix; At + sizeof (EFI_DEVICE_PATH_PROTOCOL) <= Size;) {
    EFI_DEVICE_PATH_PROTOCOL *Node = (VOID *)((UINT8 *)Path + At);
    UINTN Bytes = DevicePathNodeLength (Node);
    if (Bytes < sizeof *Node || Bytes > Size - At) { return EFI_DEVICE_ERROR; }
    if (IsDevicePathEnd (Node)) { return Found ? EFI_SUCCESS : EFI_DEVICE_ERROR; }
    if (DevicePathType (Node) == MESSAGING_DEVICE_PATH && DevicePathSubType (Node) == MSG_UFS_DP) {
      if (Found || Bytes != sizeof (UFS_DEVICE_PATH)) { return EFI_DEVICE_ERROR; }
      *Lun = ((UFS_DEVICE_PATH *)Node)->Lun;
      if (*Lun >= 8) { return EFI_DEVICE_ERROR; }
      Found = TRUE;
    }
    if (DevicePathType (Node) == MEDIA_DEVICE_PATH && DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP) {
      if (!Found || Partition->Present || Bytes != sizeof (HARDDRIVE_DEVICE_PATH)) { return EFI_DEVICE_ERROR; }
      HARDDRIVE_DEVICE_PATH *Hd = (VOID *)Node;
      if (Hd->SignatureType != SIGNATURE_TYPE_GUID || !Hd->PartitionSize || Hd->PartitionStart > MAX_UINT64 - Hd->PartitionSize) { return EFI_DEVICE_ERROR; }
      Partition->Present = TRUE; Partition->Start = Hd->PartitionStart; Partition->Blocks = Hd->PartitionSize;
      Partition->Number = Hd->PartitionNumber; CopyMem (Partition->Signature, Hd->Signature, sizeof Partition->Signature);
    }
    At += Bytes;
  }
  return EFI_DEVICE_ERROR;
}

STATIC BOOLEAN SameMedia (EFI_BLOCK_IO_PROTOCOL *Io, CONST EFI_BLOCK_IO_MEDIA *Expected) {
  return Io->Media != NULL && Io->Media->ReadOnly && Io->Media->MediaPresent &&
    Io->Media->MediaId == Expected->MediaId && Io->Media->BlockSize == Expected->BlockSize &&
    Io->Media->LastBlock == Expected->LastBlock;
}

STATIC EFI_STATUS ReadSample (EFI_BLOCK_IO_PROTOCOL *Io, VOID *Buffer, CONST EFI_BLOCK_IO_MEDIA *Expected, UINT8 Lun,
  EFI_LBA Lba, CONST CHAR8 *Token) {
  UINT8 Digest[SHA256_DIGEST_SIZE]; CHAR8 Hex[SHA256_DIGEST_SIZE * 2 + 1];
  CONST CHAR8 Alphabet[] = "0123456789abcdef";
  if (!SameMedia (Io, Expected)) { return EFI_MEDIA_CHANGED; }
  EFI_STATUS Status = Io->ReadBlocks (Io, Expected->MediaId, Lba, Expected->BlockSize, Buffer);
  if (EFI_ERROR (Status)) { return Status; }
  if (!SameMedia (Io, Expected)) { return EFI_MEDIA_CHANGED; }
  if (!Sha256HashAll (Buffer, Expected->BlockSize, Digest)) { return EFI_DEVICE_ERROR; }
  for (UINTN I = 0; I < sizeof Digest; I++) {
    Hex[I * 2] = Alphabet[Digest[I] >> 4]; Hex[I * 2 + 1] = Alphabet[Digest[I] & 15];
  }
  Hex[sizeof Hex - 1] = 0;
  DEBUG ((DEBUG_WARN, "DALI-UFS-RO: run=%a SAMPLE lun=%u lba=%Lu bytes=%u sha256=%a\n",
    Token, Lun, Lba, Expected->BlockSize, Hex));
  return EFI_SUCCESS;
}

EFI_STATUS DaliUfsReadOnlyDiagnostics (EFI_HANDLE Controller, CONST CHAR8 *Token, CONST volatile UINT64 *CompletedSync) {
  EFI_DEVICE_PATH_PROTOCOL *Parent;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Roots = 0, Partitions = 0, Samples = 0;
  UINT32 SeenLuns = 0;
  BOOLEAN KnownBootRead = FALSE;
  if (Controller == NULL || Token == NULL || CompletedSync == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = gBS->HandleProtocol (Controller, &gEfiDevicePathProtocolGuid, (VOID **)&Parent);
  DEBUG ((DEBUG_WARN, "DALI-UFS-RO: run=%a BEGIN version=1\n", Token));
  if (EFI_ERROR (Status)) { goto Done; }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { goto Done; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_DEVICE_PATH_PROTOCOL *Path; EFI_BLOCK_IO_PROTOCOL *Io; UINT8 Lun = 0;
    PARTITION_PATH Partition;
    ZeroMem (&Partition, sizeof Partition);
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiDevicePathProtocolGuid, (VOID **)&Path))) { continue; }
    Status = MediaLun (Parent, Path, &Lun, &Partition);
    if (Status == EFI_NOT_FOUND) { Status = EFI_SUCCESS; continue; }
    if (EFI_ERROR (Status)) { break; }
    Status = gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Status) || Io == NULL || Io->Media == NULL) { Status = EFI_DEVICE_ERROR; break; }
    EFI_BLOCK_IO_MEDIA *Media = Io->Media;
    DEBUG ((DEBUG_WARN, "DALI-UFS-RO: run=%a MEDIA lun=%u partition=%u readonly=%u block_size=%u last_lba=%Lu\n",
      Token, Lun, Media->LogicalPartition, Media->ReadOnly, Media->BlockSize, Media->LastBlock));
    if (!Media->ReadOnly || !Media->MediaPresent) { Status = EFI_ACCESS_DENIED; break; }
    if (Media->LogicalPartition != Partition.Present) { Status = EFI_DEVICE_ERROR; break; }
    if (Media->LogicalPartition) {
      CHAR8 Signature[33]; CONST CHAR8 Alphabet[] = "0123456789abcdef";
      if (Media->LastBlock == MAX_UINT64 || Partition.Blocks != Media->LastBlock + 1) { Status = EFI_DEVICE_ERROR; break; }
      for (UINTN J = 0; J < sizeof Partition.Signature; J++) {
        Signature[J * 2] = Alphabet[Partition.Signature[J] >> 4]; Signature[J * 2 + 1] = Alphabet[Partition.Signature[J] & 15];
      }
      Signature[32] = 0;
      DEBUG ((DEBUG_WARN, "DALI-UFS-RO: run=%a PARTITION lun=%u number=%u start_lba=%Lu blocks=%Lu signature_le=%a\n",
        Token, Lun, Partition.Number, Partition.Start, Partition.Blocks, Signature));
      ++Partitions; continue;
    }
    if (SeenLuns & (1U << Lun)) { Status = EFI_DEVICE_ERROR; break; }
    SeenLuns |= 1U << Lun;
    if (Media->BlockSize < 512 || Media->BlockSize > 65536 || (Media->BlockSize & (Media->BlockSize - 1)) ||
        Media->LastBlock < 1 || Media->LastBlock == MAX_UINT64 || Media->IoAlign > 65536 ||
        (Media->IoAlign > 1 && (Media->IoAlign & (Media->IoAlign - 1)))) { Status = EFI_UNSUPPORTED; break; }
    UINTN Alignment = Media->IoAlign > EFI_PAGE_SIZE ? Media->IoAlign : EFI_PAGE_SIZE;
    EFI_BLOCK_IO_MEDIA Snapshot = *Media;
    VOID *Buffer = AllocateAlignedPages (EFI_SIZE_TO_PAGES (Snapshot.BlockSize), Alignment);
    if (Buffer == NULL) { Status = EFI_OUT_OF_RESOURCES; break; }
    ++Roots;
    EFI_LBA Lbas[3] = { 0, 1, Snapshot.LastBlock };
    Print (L"UFS: BlockIo read LUN=%u\r\n", Lun);
    for (UINTN Sample = 0; Sample < 3; Sample++) {
      Status = ReadSample (Io, Buffer, &Snapshot, Lun, Lbas[Sample], Token);
      if (EFI_ERROR (Status)) {
        Print (L"UFS: BlockIo read LUN=%u sample=%u result=%r\r\n", Lun, Sample, Status);
        break;
      }
      ++Samples;
    }
    if (!EFI_ERROR (Status) && Lun == BASELINE_LUN && Snapshot.BlockSize == BASELINE_BLOCK_SIZE && Snapshot.LastBlock == BASELINE_LAST_LBA) {
      Status = ReadSample (Io, Buffer, &Snapshot, Lun, BASELINE_BOOT_B_LBA, Token);
      if (!EFI_ERROR (Status)) { KnownBootRead = TRUE; ++Samples; }
    }
    if (!EFI_ERROR (Status)) {
      UINT64 Before = CompletedSync[Lun];
      Print (L"UFS: BlockIo sync LUN=%u\r\n", Lun);
      Status = SameMedia (Io, &Snapshot) ? Io->FlushBlocks (Io) : EFI_MEDIA_CHANGED;
      if (!EFI_ERROR (Status) && !SameMedia (Io, &Snapshot)) { Status = EFI_MEDIA_CHANGED; }
      if (!EFI_ERROR (Status) && (Before == MAX_UINT64 || CompletedSync[Lun] != Before + 1)) { Status = EFI_DEVICE_ERROR; }
      Print (L"UFS: BlockIo sync LUN=%u result=%r\r\n", Lun, Status);
      DEBUG ((DEBUG_WARN, "DALI-UFS-RO: run=%a SYNC lun=%u status=%lx\n", Token, Lun, (UINT64)Status));
    }
    FreeAlignedPages (Buffer, EFI_SIZE_TO_PAGES (Snapshot.BlockSize));
    if (EFI_ERROR (Status)) { break; }
  }
  if (!EFI_ERROR (Status) && (!Roots || !KnownBootRead)) { Status = EFI_NOT_FOUND; }
Done:
  if (Handles != NULL) { gBS->FreePool (Handles); }
  DEBUG ((DEBUG_WARN, "DALI-UFS-RO: run=%a END status=%lx roots=%lu partitions=%lu samples=%lu\n",
    Token, (UINT64)Status, Roots, Partitions, Samples));
  return Status;
}
