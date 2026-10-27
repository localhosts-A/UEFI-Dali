// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/Mt6991UfsDmaLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/ScsiIo.h>
#include "UfsScratch.h"
#include "UfsLayout.h"
#include "UfsSlotScan.h"

typedef struct {
  EFI_BLOCK_IO_PROTOCOL Io;
  EFI_BLOCK_IO_MEDIA Media;
  EFI_HANDLE Handle;
  EFI_HANDLE ParentHandle;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_BLOCK_IO_PROTOCOL *Parent;
  EFI_SCSI_IO_PROTOCOL *Scsi;
  CONST MT6991_UFS_SCRATCH_GRANT *LiveGrant;
  MT6991_UFS_SCRATCH_GRANT Grant;
  CONST MT6991_UFS_GEOMETRY *Geometry;
  CONST volatile BOOLEAN *HostClosed;
  CONST volatile UINT64 *CompletedSync;
  VOID *Sense;
  UINT32 ParentAlign, ScsiAlign;
  BOOLEAN Attempted, Closed, Busy, Faulted, CloseRequested;
} DALI_UFS_SCRATCH;
STATIC DALI_UFS_SCRATCH mScratch;

STATIC BOOLEAN Aligned (UINT32 Align) {
  return Align <= EFI_PAGE_SIZE && (Align == 0 || (Align & (Align - 1)) == 0);
}

STATIC BOOLEAN SameMedia (VOID) {
  // Reacquire the protocols before dereferencing cached interfaces. A separate
  // application may have disconnected this disk since the previous request.
  // Like the host, this consumer supports only serialized BSP execution.
  EFI_BLOCK_IO_PROTOCOL *Parent = NULL;
  EFI_SCSI_IO_PROTOCOL *Scsi = NULL;
  if (gBS->HandleProtocol (mScratch.ParentHandle, &gEfiBlockIoProtocolGuid, (VOID **)&Parent) != EFI_SUCCESS ||
      Parent != mScratch.Parent ||
      gBS->HandleProtocol (mScratch.ParentHandle, &gEfiScsiIoProtocolGuid, (VOID **)&Scsi) != EFI_SUCCESS ||
      Scsi != mScratch.Scsi) { return FALSE; }
  CONST MT6991_UFS_SCRATCH_GRANT *G = &mScratch.Grant;
  CONST MT6991_UFS_GEOMETRY *Geometry = &mScratch.Geometry[G->Lun];
  CONST EFI_BLOCK_IO_MEDIA *M = mScratch.Parent->Media;
  return CompareMem (G, mScratch.LiveGrant, sizeof *G) == 0 &&
    Geometry->Valid && Geometry->Blocks == G->CapacityBlocks && Geometry->BlockSize == G->BlockSize &&
    Geometry->WriteProtectionKnown && !Geometry->WriteProtected &&
    M != NULL && M->MediaPresent && M->ReadOnly && !M->LogicalPartition &&
    M->MediaId == mScratch.Media.MediaId && M->BlockSize == G->BlockSize &&
    M->LastBlock == G->CapacityBlocks - 1 && M->IoAlign == mScratch.ParentAlign &&
    mScratch.Scsi->IoAlign == mScratch.ScsiAlign;
}

STATIC EFI_STATUS Enter (EFI_BLOCK_IO_PROTOCOL *This) {
  if (This != &mScratch.Io || This->Media != &mScratch.Media) { return EFI_INVALID_PARAMETER; }
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = EFI_SUCCESS;
  if (Tpl != TPL_APPLICATION) { Status = EFI_UNSUPPORTED; }
  else if (mScratch.Closed || mScratch.HostClosed == NULL || *mScratch.HostClosed) { Status = EFI_NO_MEDIA; }
  else if (mScratch.Faulted) { Status = EFI_DEVICE_ERROR; }
  else if (mScratch.Busy) { Status = EFI_NOT_READY; }
  else if (!SameMedia ()) { mScratch.Faulted = TRUE; Status = EFI_MEDIA_CHANGED; }
  else { mScratch.Busy = TRUE; }
  gBS->RestoreTPL (Tpl); return Status;
}

STATIC EFI_STATUS Leave (EFI_STATUS Status, BOOLEAN Submitted) {
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Status != EFI_SUCCESS && Submitted) { mScratch.Faulted = TRUE; }
  mScratch.Busy = FALSE;
  gBS->RestoreTPL (Tpl); return Status;
}

STATIC EFI_STATUS Execute (UINT8 *Cdb, UINTN CdbBytes, VOID *Buffer, UINT32 Bytes, BOOLEAN Write) {
  EFI_SCSI_IO_SCSI_REQUEST_PACKET P;
  ZeroMem (&P, sizeof P); ZeroMem (mScratch.Sense, 32);
  P.Timeout = MT6991_UFS_COMMAND_TIMEOUT_100NS;
  P.Cdb = Cdb; P.CdbLength = (UINT8)CdbBytes;
  P.SenseData = mScratch.Sense; P.SenseDataLength = 32;
  // Pinned Mu's synchronous UFS path leaves the zero-initialized host status
  // unchanged on success; it does explicitly return TargetStatus. A nonzero
  // adapter status, missing target completion or lower error still fails.
  P.TargetStatus = MAX_UINT8;
  P.DataDirection = Write ? EFI_SCSI_IO_DATA_DIRECTION_WRITE : EFI_SCSI_IO_DATA_DIRECTION_READ;
  if (Write) { P.OutDataBuffer = Buffer; P.OutTransferLength = Bytes; }
  else { P.InDataBuffer = Buffer; P.InTransferLength = Bytes; }
  // Exactly one submission. In particular, never retry partial/uncertain writes
  // or SYNCHRONIZE CACHE. The host's SCSI and doorbell guards run underneath.
  EFI_STATUS Status = mScratch.Scsi->ExecuteScsiCommand (mScratch.Scsi, &P, NULL);
  if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
  if (P.HostAdapterStatus != 0 || P.TargetStatus != 0 ||
      P.InTransferLength != (Write ? 0 : Bytes) || P.OutTransferLength != (Write ? Bytes : 0)) {
    return EFI_DEVICE_ERROR;
  }
  return !mScratch.Closed && !*mScratch.HostClosed && SameMedia () ? EFI_SUCCESS : EFI_MEDIA_CHANGED;
}

STATIC EFI_STATUS Transfer (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Bytes, VOID *Buffer, BOOLEAN Write) {
  EFI_STATUS Status = Enter (This);
  if (Status != EFI_SUCCESS) { return Status; }
  if (MediaId != mScratch.Media.MediaId) { return Leave (EFI_MEDIA_CHANGED, FALSE); }
  if (Write && mScratch.Media.ReadOnly) { return Leave (EFI_WRITE_PROTECTED, FALSE); }
  if (Bytes == 0) { return Leave (EFI_SUCCESS, FALSE); }
  if (Buffer == NULL || ((UINTN)Buffer & (mScratch.Media.IoAlign - 1)) != 0) {
    return Leave (EFI_INVALID_PARAMETER, FALSE);
  }
  UINT64 Absolute, Count;
  Status = Mt6991UfsScratchMapRange (mScratch.Geometry, &mScratch.Grant,
    mScratch.Grant.Lun, Lba, Bytes, Write, &Absolute, &Count);
  if (Status != EFI_SUCCESS) { return Leave (Status, FALSE); }
  // The child may expose a larger bounded log slot than the DMA engine accepts
  // in one command. Split it into aligned, individually policy-checked SCSI
  // transfers; stop at the first error and never replay a submitted chunk.
  if (MT6991_UFS_DMA_MAX_TRANSFER < mScratch.Grant.BlockSize) {
    return Leave (EFI_BAD_BUFFER_SIZE, FALSE);
  }
  UINTN MaximumChunk = MT6991_UFS_DMA_MAX_TRANSFER -
    (MT6991_UFS_DMA_MAX_TRANSFER % mScratch.Grant.BlockSize);
  if (MaximumChunk == 0) { return Leave (EFI_BAD_BUFFER_SIZE, FALSE); }
  UINTN Offset = 0;
  BOOLEAN Submitted = FALSE;
  while (Offset < Bytes) {
    UINTN ChunkBytes = Bytes - Offset;
    if (ChunkBytes > MaximumChunk) { ChunkBytes = MaximumChunk; }
    UINT64 ChunkLba = Absolute + Offset / mScratch.Grant.BlockSize;
    UINT64 ChunkBlocks = ChunkBytes / mScratch.Grant.BlockSize;
    UINT8 Cdb[16]; UINTN CdbBytes;
    Status = Mt6991UfsBuildReadWriteCdb (Write, ChunkLba, ChunkBlocks, Cdb, &CdbBytes);
    if (Status != EFI_SUCCESS) { return Leave (Status, Submitted); }
    Submitted = TRUE;
    Status = Execute (Cdb, CdbBytes, (UINT8 *)Buffer + Offset, (UINT32)ChunkBytes, Write);
    if (Status != EFI_SUCCESS) { return Leave (Status, Submitted); }
    Offset += ChunkBytes;
  }
  return Leave (EFI_SUCCESS, Submitted);
}

STATIC EFI_STATUS EFIAPI ReadBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Bytes, VOID *Buffer) { return Transfer (This, MediaId, Lba, Bytes, Buffer, FALSE); }
STATIC EFI_STATUS EFIAPI WriteBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN Bytes, VOID *Buffer) { return Transfer (This, MediaId, Lba, Bytes, Buffer, TRUE); }
STATIC EFI_STATUS EFIAPI Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification) {
  (VOID)ExtendedVerification;
  return This == &mScratch.Io ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI FlushBlocks (EFI_BLOCK_IO_PROTOCOL *This) {
  EFI_STATUS Status = Enter (This);
  if (Status != EFI_SUCCESS) { return Status; }
  UINT64 Before = mScratch.CompletedSync[mScratch.Grant.Lun];
  if (Before == MAX_UINT64) { return Leave (EFI_DEVICE_ERROR, FALSE); }
  UINT8 Cdb[10] = { 0x35 }; // IMMED=0, synchronize the LU, no data direction.
  Status = Execute (Cdb, sizeof Cdb, NULL, 0, FALSE);
  if (Status == EFI_SUCCESS && mScratch.CompletedSync[mScratch.Grant.Lun] != Before + 1) { Status = EFI_DEVICE_ERROR; }
  return Leave (Status, TRUE);
}

STATIC BOOLEAN RootLun (CONST EFI_DEVICE_PATH_PROTOCOL *Parent, CONST EFI_DEVICE_PATH_PROTOCOL *Path, UINT8 Lun) {
  UINTN ParentSize = GetDevicePathSize (Parent);
  if (ParentSize < sizeof (EFI_DEVICE_PATH_PROTOCOL)) { return FALSE; }
  UINTN Prefix = ParentSize - sizeof (EFI_DEVICE_PATH_PROTOCOL);
  if (GetDevicePathSize (Path) != Prefix + sizeof (UFS_DEVICE_PATH) + sizeof (EFI_DEVICE_PATH_PROTOCOL) ||
      CompareMem (Parent, Path, Prefix) != 0) { return FALSE; }
  CONST UFS_DEVICE_PATH *Ufs = (CONST VOID *)((CONST UINT8 *)Path + Prefix);
  CONST EFI_DEVICE_PATH_PROTOCOL *End = (CONST VOID *)((CONST UINT8 *)Ufs + sizeof *Ufs);
  return DevicePathType (&Ufs->Header) == MESSAGING_DEVICE_PATH &&
    DevicePathSubType (&Ufs->Header) == MSG_UFS_DP && DevicePathNodeLength (&Ufs->Header) == sizeof *Ufs &&
    Ufs->Pun == 0 && Ufs->Lun == Lun && IsDevicePathEnd (End) && DevicePathNodeLength (End) == sizeof *End;
}

STATIC EFI_STATUS ReadLayout (VOID *Context, UINT8 Lun, UINT64 Lba, UINTN Bytes, VOID *Buffer) {
  (VOID)Context;
  if (!mScratch.Busy || Lun != mScratch.Grant.Lun || mScratch.CloseRequested ||
      *mScratch.HostClosed || !SameMedia ()) {
    return EFI_MEDIA_CHANGED;
  }
  EFI_STATUS Status = mScratch.Parent->ReadBlocks (mScratch.Parent, mScratch.Media.MediaId, Lba, Bytes, Buffer);
  if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
  return !mScratch.CloseRequested && !*mScratch.HostClosed && SameMedia () ? EFI_SUCCESS : EFI_MEDIA_CHANGED;
}

EFI_STATUS DaliUfsScratchPublishChecked (EFI_HANDLE Controller,
  CONST MT6991_UFS_SCRATCH_GRANT *Grant, CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST volatile BOOLEAN *Closed, CONST volatile UINT64 *CompletedSync,
  DALI_UFS_SCRATCH_AUTHORIZE Authorize, VOID *Context) {
  // Default-off return precedes all Boot Services use and all device access.
  if (Grant == NULL || !Grant->Enabled) { return EFI_NOT_STARTED; }
  if (Authorize == NULL) { return EFI_ACCESS_DENIED; }
  if (EfiGetCurrentTpl () != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  if (mScratch.Attempted) { return EFI_ALREADY_STARTED; }
  if (Controller == NULL || Closed == NULL || *Closed || CompletedSync == NULL) { return EFI_ACCESS_DENIED; }
  UINT64 Absolute, Count;
  EFI_STATUS Status = Mt6991UfsScratchMapRange (Geometry, Grant, Grant->Lun, 0, Grant->BlockSize, TRUE, &Absolute, &Count);
  if (Status != EFI_SUCCESS) { return Status; }
  mScratch.Attempted = TRUE; mScratch.Closed = TRUE;
  CopyMem (&mScratch.Grant, Grant, sizeof *Grant); mScratch.LiveGrant = Grant; mScratch.Geometry = Geometry;
  mScratch.HostClosed = Closed; mScratch.CompletedSync = CompletedSync;
  EFI_DEVICE_PATH_PROTOCOL *ParentPath = NULL, *RootPath = NULL;
  EFI_HANDLE *Handles = NULL; UINTN HandlesCount = 0, Matches = 0;
  Status = gBS->HandleProtocol (Controller, &gEfiDevicePathProtocolGuid, (VOID **)&ParentPath);
  if (Status != EFI_SUCCESS || ParentPath == NULL) { return EFI_NOT_FOUND; }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandlesCount, &Handles);
  if (Status != EFI_SUCCESS) { return Status; }
  for (UINTN Index = 0; Index < HandlesCount; ++Index) {
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    if (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path) != EFI_SUCCESS ||
        Path == NULL || !RootLun (ParentPath, Path, Grant->Lun)) { continue; }
    ++Matches;
    mScratch.ParentHandle = Handles[Index];
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&mScratch.Parent);
    if (Status == EFI_SUCCESS) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiScsiIoProtocolGuid, (VOID **)&mScratch.Scsi);
    }
    RootPath = Path;
    if (Status != EFI_SUCCESS) { break; }
  }
  gBS->FreePool (Handles);
  if (Status != EFI_SUCCESS) { return Status; }
  if (Matches != 1 || mScratch.Parent == NULL || mScratch.Parent->Media == NULL || mScratch.Scsi == NULL) { return EFI_NOT_FOUND; }
  mScratch.ParentAlign = mScratch.Parent->Media->IoAlign; mScratch.ScsiAlign = mScratch.Scsi->IoAlign;
  if (!Aligned (mScratch.ParentAlign) || !Aligned (mScratch.ScsiAlign) ||
      mScratch.Scsi->ExecuteScsiCommand == NULL || mScratch.Parent->ReadBlocks == NULL) { return EFI_UNSUPPORTED; }
  ZeroMem (&mScratch.Media, sizeof mScratch.Media);
  mScratch.Media.MediaId = mScratch.Parent->Media->MediaId;
  mScratch.Media.MediaPresent = TRUE; mScratch.Media.LogicalPartition = TRUE;
  mScratch.Media.WriteCaching = TRUE; mScratch.Media.BlockSize = Grant->BlockSize;
  mScratch.Media.LastBlock = Grant->ScratchBlocks - 1;
  mScratch.Media.IoAlign = mScratch.ParentAlign > mScratch.ScsiAlign ? mScratch.ParentAlign : mScratch.ScsiAlign;
  if (mScratch.Media.IoAlign == 0) { mScratch.Media.IoAlign = 1; }
  if (!SameMedia ()) { return EFI_MEDIA_CHANGED; }
  // Never publish an expected grant solely on its shape. Read the real disk's
  // reciprocal GPT and compare the observed target partition and bounded slot.
  VOID *Workspace = AllocateAlignedPages (EFI_SIZE_TO_PAGES (DALI_UFS_LAYOUT_WORK_BYTES), EFI_PAGE_SIZE);
  if (Workspace == NULL) { return EFI_OUT_OF_RESOURCES; }
  mScratch.Busy = TRUE;
  DALI_UFS_LAYOUT Layout;
  Status = DaliUfsInspectScratchLayout (Geometry, &mScratch.Grant, ReadLayout, NULL,
    Workspace, DALI_UFS_LAYOUT_WORK_BYTES, &Layout);
  if (Status == EFI_SUCCESS && Layout.Target == MT6991_UFS_TARGET_USERDATA_LOGS) {
    // Keep shutdown admission fenced across the complete partition scan too;
    // CloseAdmission must not stop or disconnect the parent during a read.
    Status = DaliUfsSelectEmptyUserdataPair (&Layout, mScratch.Grant.Sequence,
      mScratch.ParentAlign, mScratch.ScsiAlign, ReadLayout, NULL);
  }
  mScratch.Busy = FALSE;
  FreeAlignedPages (Workspace, EFI_SIZE_TO_PAGES (DALI_UFS_LAYOUT_WORK_BYTES));
  if (Status != EFI_SUCCESS) { return Status; }
  if (!Layout.Verified || mScratch.CloseRequested || *mScratch.HostClosed || !SameMedia ()) { return EFI_MEDIA_CHANGED; }
  if (Authorize != NULL) {
    CONST MT6991_UFS_SCRATCH_GRANT *Live = NULL;
    mScratch.Busy = TRUE;
    Status = Authorize (Context, &mScratch.Grant, &Layout, &Live);
    mScratch.Busy = FALSE;
    if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_SECURITY_VIOLATION; }
    if (Live == NULL || mScratch.CloseRequested || *mScratch.HostClosed) { return EFI_SECURITY_VIOLATION; }
    if (mScratch.Grant.Target == MT6991_UFS_TARGET_USERDATA_LOGS) {
      MT6991_UFS_SCRATCH_GRANT Selected = mScratch.Grant;
      Selected.Sequence = Layout.Sequence;
      Selected.ScratchStartLba = Layout.ScratchStartLba;
      Selected.TailGuardBlocks = Layout.TailGuardBlocks;
      if (!Mt6991UfsScratchGrantValid (Live) || CompareMem (Live, &Selected, sizeof Selected)) {
        return EFI_SECURITY_VIOLATION;
      }
      CopyMem (&mScratch.Grant, Live, sizeof mScratch.Grant);
    } else if (CompareMem (Live, &mScratch.Grant, sizeof *Live)) {
      return EFI_SECURITY_VIOLATION;
    }
    mScratch.LiveGrant = Live;
    if (!SameMedia ()) { return EFI_MEDIA_CHANGED; }
  }
  VENDOR_DEVICE_PATH Node = { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof Node, 0 } },
    { 0x3f625e8c, 0x56d1, 0x4134, { 0x88, 0x74, 0x72, 0x61, 0x6e, 0x67, 0x65, 0x01 } } };
  mScratch.Path = AppendDevicePathNode (RootPath, &Node.Header);
  mScratch.Sense = AllocateAlignedPages (1, EFI_PAGE_SIZE);
  if (mScratch.Path == NULL || mScratch.Sense == NULL) { Status = EFI_OUT_OF_RESOURCES; }
  else if (mScratch.CloseRequested || *mScratch.HostClosed || !SameMedia ()) { Status = EFI_MEDIA_CHANGED; }
  else {
    mScratch.Io = (EFI_BLOCK_IO_PROTOCOL){ EFI_BLOCK_IO_PROTOCOL_REVISION, &mScratch.Media, Reset, ReadBlocks, WriteBlocks, FlushBlocks };
    Status = gBS->InstallMultipleProtocolInterfaces (&mScratch.Handle,
      &gEfiDevicePathProtocolGuid, mScratch.Path, &gEfiBlockIoProtocolGuid, &mScratch.Io, NULL);
  }
  if (Status != EFI_SUCCESS) {
    if (mScratch.Path != NULL) { FreePool (mScratch.Path); mScratch.Path = NULL; }
    if (mScratch.Sense != NULL) { FreeAlignedPages (mScratch.Sense, 1); mScratch.Sense = NULL; }
    return Status;
  }
  // Installation may dispatch an application callback which requests shutdown.
  // Keep the child closed and remove it (or retain it closed on failure).
  if (mScratch.CloseRequested || *mScratch.HostClosed || !SameMedia ()) {
    DaliUfsScratchUnpublish (); return EFI_MEDIA_CHANGED;
  }
  mScratch.Closed = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS DaliUfsScratchCloseAdmission (VOID) {
  mScratch.CloseRequested = TRUE; mScratch.Closed = TRUE; mScratch.Media.MediaPresent = FALSE;
  return mScratch.Busy ? EFI_NOT_READY : EFI_SUCCESS;
}

EFI_STATUS DaliUfsScratchOwnedIo (CONST MT6991_UFS_SCRATCH_GRANT *Live,
  EFI_BLOCK_IO_PROTOCOL **Io) {
  if (Io == NULL) { return EFI_INVALID_PARAMETER; }
  *Io = NULL;
  if (EfiGetCurrentTpl () != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  if (Live == NULL || Live != mScratch.LiveGrant || !Live->Enabled ||
      (Live->Target != MT6991_UFS_TARGET_EXPDB && Live->Target != MT6991_UFS_TARGET_USERDATA_LOGS) || mScratch.Handle == NULL ||
      mScratch.Closed || mScratch.CloseRequested || mScratch.Busy || mScratch.Faulted ||
      mScratch.HostClosed == NULL || *mScratch.HostClosed ||
      CompareMem (Live, &mScratch.Grant, sizeof *Live)) { return EFI_ACCESS_DENIED; }
  EFI_BLOCK_IO_PROTOCOL *Published = NULL;
  if (gBS->HandleProtocol (mScratch.Handle, &gEfiBlockIoProtocolGuid, (VOID **)&Published) != EFI_SUCCESS ||
      Published != &mScratch.Io || !SameMedia ()) { return EFI_MEDIA_CHANGED; }
  *Io = &mScratch.Io; return EFI_SUCCESS;
}

EFI_STATUS DaliUfsScratchSealReadOnly (CONST MT6991_UFS_SCRATCH_GRANT *Live) {
  EFI_BLOCK_IO_PROTOCOL *Io = NULL;
  EFI_STATUS Status = DaliUfsScratchOwnedIo (Live, &Io);
  if (Status != EFI_SUCCESS) { return Status; }
  EFI_TPL Old = gBS->RaiseTPL (TPL_NOTIFY);
  if (mScratch.Closed || mScratch.Busy || *mScratch.HostClosed || !SameMedia ()) { Status = EFI_MEDIA_CHANGED; }
  else { mScratch.Media.ReadOnly = TRUE; }
  gBS->RestoreTPL (Old); return Status;
}

EFI_STATUS DaliUfsScratchUnpublish (VOID) {
  if (EfiGetCurrentTpl () != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = DaliUfsScratchCloseAdmission ();
  gBS->RestoreTPL (Tpl);
  if (Status != EFI_SUCCESS) { return Status; }
  if (mScratch.Handle == NULL) { return EFI_SUCCESS; }
  Status = gBS->UninstallMultipleProtocolInterfaces (mScratch.Handle,
    &gEfiDevicePathProtocolGuid, mScratch.Path, &gEfiBlockIoProtocolGuid, &mScratch.Io, NULL);
  // Retain all published storage if another consumer prevents uninstallation.
  // Callbacks remain closed, including after the parent has been disconnected.
  if (Status != EFI_SUCCESS) { return Status; }
  mScratch.Handle = NULL;
  FreePool (mScratch.Path); mScratch.Path = NULL;
  FreeAlignedPages (mScratch.Sense, 1); mScratch.Sense = NULL;
  return EFI_SUCCESS;
}
