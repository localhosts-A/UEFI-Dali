// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/Mt6991UfsController.h>
#include <Library/Mt6991UfsPolicy.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DaliUfsDiagnostic.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/UniXNUStorageQuiesce.h>
#include "UfsHostInternal.h"
#include "UfsShutdown.h"
#include "UfsScratch.h"
#include "UfsIdentity.h"

#define HCI 0x16810000U
#define WINDOW 0x2A00U
#define COMMAND_TIMEOUT MT6991_UFS_COMMAND_TIMEOUT_100NS
STATIC MT6991_UFS_CONTROLLER mControl;
STATIC MT6991_UFS_DMA mDma;
STATIC MT6991_UFS_GEOMETRY mGeometry[8];
// No runtime path populates a scratch grant yet; all write submissions remain denied.
STATIC MT6991_UFS_SCRATCH_GRANT mScratchGrant;
STATIC DALI_UFS_IDENTITY mIdentity;
STATIC MT6991_UFS_PENDING mPending;
STATIC volatile UINT64 mCompletedSync[8];
STATIC EFI_HANDLE mController;
STATIC BOOLEAN mAttempted, mClosed = TRUE, mInitUsed, mInitFinished, mDisconnected;
STATIC UINT32 mTrl, mTmrl;
STATIC DALI_UFS_SHUTDOWN mShutdown;
STATIC EFI_STATUS mQuiesceFailure;
STATIC BOOLEAN mQuiesced, mQuiescing;
STATIC UNIXNU_STORAGE_QUIESCE_PROTOCOL mStorage;

STATIC UINT32 IoRead (UINTN Address, VOID *Context) { (VOID)Context; return MmioRead32 (Address); }
STATIC BOOLEAN IoWrite (UINTN Address, UINT32 Value, VOID *Context) {
  // Control-plane breadcrumbs precede command submission, never the completion
  // polling loop. They use existing arguments and add no diagnostic MMIO reads.
  if (Address == HCI + 0x34) { Print (L"UFS: control HCE=%x\r\n", Value); }
  if (Address == HCI + 0x90) { Print (L"UFS: control UIC=%x\r\n", Value); }
  (VOID)Context; MmioWrite32 (Address, Value); MemoryFence (); return TRUE;
}
STATIC VOID Fence (VOID *Context) { (VOID)Context; MemoryFence (); }
STATIC VOID Stall (UINT32 Us, VOID *Context) { (VOID)Context; gBS->Stall (Us); }
STATIC UINT64 Sip (UINT32 Service, UINT32 Selector, UINT32 A, UINT32 B, VOID *Context) {
  ARM_SMC_ARGS Args;
  (VOID)Context; ZeroMem (&Args, sizeof Args);
  Args.Arg0 = Service; Args.Arg1 = Selector; Args.Arg2 = A; Args.Arg3 = B;
  Print (L"UFS: SiP selector=%x args=%x/%x\r\n", Selector, A, B);
  ArmCallSmc (&Args);
  Print (L"UFS: SiP returned=%lx\r\n", Args.Arg0);
  return Args.Arg0;
}

STATIC EFI_STATUS StopController (VOID) {
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  mClosed = TRUE;
  Mt6991UfsDmaPoison (&mDma);
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  if (Mt6991UfsStop (&mControl) == Mt6991UfsStopped) {
    Status = mDma.Initialized ? Mt6991UfsDmaStopped (&mDma, MmioRead32 (HCI + 0x34)) : EFI_SUCCESS;
  }
  gBS->RestoreTPL (OldTpl); return Status;
}

STATIC EFI_STATUS EFIAPI Quiesce (VOID) {
  if (EfiGetCurrentTpl () > TPL_NOTIFY) { return EFI_UNSUPPORTED; }
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mQuiesced || mQuiescing) {
    EFI_STATUS Status = mQuiesced ? EFI_SUCCESS : EFI_NOT_READY;
    gBS->RestoreTPL (OldTpl); return Status;
  }
  if (OldTpl != TPL_APPLICATION) { gBS->RestoreTPL (OldTpl); return EFI_UNSUPPORTED; }
  EFI_STATUS Status;
  mQuiescing = TRUE;
  Status = DaliUfsBootStageCloseAdmission ();
  if (Status != EFI_SUCCESS) {
    mQuiescing = FALSE; gBS->RestoreTPL (OldTpl); return Status;
  }
  Status = DaliUfsScratchCloseAdmission ();
  if (Status != EFI_SUCCESS) {
    // Do not close the lower completion path or disconnect a live ScsiIo call.
    // The child stays closed; a later quiesce may clean up after it returns.
    mQuiescing = FALSE; gBS->RestoreTPL (OldTpl); return Status;
  }
  // Admission closes before any poll/allocation. Private shutdown commands use
  // the owned-DMA core directly, never reopen the public protocol callbacks.
  // Host submission/DMA callbacks use this same TPL on the firmware BSP. No MP caller
  // is supported. Keep it raised through drain/stop; disconnect needs the
  // caller's TPL and may legitimately invoke the serialized Free/Unmap callbacks.
  mClosed = TRUE;
  Status = DaliUfsDrainAndSync (&mDma, &mPending, mGeometry, mCompletedSync, &mControl.Io, &mShutdown);
  if (mQuiesceFailure == EFI_SUCCESS && Status != EFI_SUCCESS) { mQuiesceFailure = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
  // Even failed sync/drain must stop/quarantine and attempt safe cleanup.
  Status = StopController ();
  gBS->RestoreTPL (OldTpl);
  EFI_STATUS ScratchStatus = DaliUfsScratchUnpublish ();
  if (mQuiesceFailure == EFI_SUCCESS && ScratchStatus != EFI_SUCCESS) {
    mQuiesceFailure = EFI_ERROR (ScratchStatus) ? ScratchStatus : EFI_DEVICE_ERROR;
  }
  if (Status == EFI_SUCCESS && mController != NULL && !mDisconnected) {
    // Let PassThru retire its own descriptor pointers/cookies before releasing
    // any residual allocations. Never leave a live protocol holding freed TRLs.
    Status = gBS->DisconnectController (mController, NULL, NULL);
    if (Status == EFI_SUCCESS || Status == EFI_NOT_FOUND) {
      VOID *PassThru = NULL;
      EFI_STATUS Present = gBS->HandleProtocol (mController, &gEfiExtScsiPassThruProtocolGuid, &PassThru);
      Status = (Present == EFI_UNSUPPORTED || Present == EFI_NOT_FOUND) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    if (Status == EFI_SUCCESS) { mDisconnected = TRUE; }
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Status == EFI_SUCCESS && mDma.Initialized) { Status = Mt6991UfsDmaReleaseAll (&mDma); }
  if (mQuiesceFailure == EFI_SUCCESS && Status != EFI_SUCCESS) { mQuiesceFailure = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
  Status = mQuiesceFailure;
  mQuiesced = Status == EFI_SUCCESS;
  mQuiescing = FALSE;
  gBS->RestoreTPL (OldTpl);
  DEBUG ((DEBUG_WARN, "DALI-UFS-HANDOFF: run=%a closed=1 drained-synced=%u mask=%x commands=%u result=%r\n",
    (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), mShutdown.Completed,
    mShutdown.SyncedLuns, mShutdown.Commands, Status));
  DEBUG ((DEBUG_WARN, "DALI-UFS-HOST: run=%a quiesce=%r hce=%x\n",
    (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), Status, mControl.Touched ? MmioRead32 (HCI + 0x34) : 0));
  return Status;
}

STATIC EFI_STATUS EFIAPI StorageQuiesce (UNIXNU_STORAGE_QUIESCE_PROTOCOL *This) {
  return This == &mStorage ? Quiesce () : EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS EFIAPI GetBar (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, UINTN *Bar);
STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL mHost;
STATIC BOOLEAN ValidHost (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This) { return This == &mHost; }
STATIC EFI_STATUS EFIAPI GetBar (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, UINTN *Bar) {
  if (!ValidHost (This) || Bar == NULL) { return EFI_INVALID_PARAMETER; }
  if (mClosed) { return EFI_DEVICE_ERROR; }
  *Bar = HCI; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Allocate (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
  EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **Host, UINT64 Attributes) {
  (VOID)Type;
  if (!ValidHost (This) || MemoryType != EfiBootServicesData || Attributes) { return EFI_UNSUPPORTED; }
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = mClosed ? EFI_DEVICE_ERROR : Mt6991UfsDmaAllocate (&mDma, Pages, Host);
  if (mDma.Poisoned && !mDma.Stopped) { StopController (); }
  gBS->RestoreTPL (OldTpl); return Status;
}
STATIC EFI_STATUS EFIAPI Free (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, UINTN Pages, VOID *Host) {
  if (!ValidHost (This)) { return EFI_INVALID_PARAMETER; }
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = Mt6991UfsDmaFree (&mDma, Pages, Host);
  if (mDma.Poisoned && !mDma.Stopped) { StopController (); }
  gBS->RestoreTPL (OldTpl); return Status;
}
STATIC EFI_STATUS EFIAPI Map (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_OPERATION Op, VOID *Host, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Device, VOID **Mapping) {
  UINT64 Id;
  if (!ValidHost (This) || Bytes == NULL || Device == NULL || Mapping == NULL || (UINT32)Op >= EdkiiUfsHcOperationMaximum) { return EFI_INVALID_PARAMETER; }
  *Mapping = NULL;
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = mClosed ? EFI_DEVICE_ERROR : Mt6991UfsDmaMap (&mDma, (MT6991_DMA_OPERATION)Op, Host, *Bytes, Device, &Id);
  if (mDma.Poisoned && !mDma.Stopped) { StopController (); }
  if (!EFI_ERROR (Status)) { *Mapping = (VOID *)(UINTN)Id; }
  gBS->RestoreTPL (OldTpl); return Status;
}
STATIC EFI_STATUS EFIAPI Unmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, VOID *Mapping) {
  if (!ValidHost (This)) { return EFI_INVALID_PARAMETER; }
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = Mt6991UfsDmaUnmap (&mDma, (UINTN)Mapping);
  if (mDma.Poisoned && !mDma.Stopped) { StopController (); }
  gBS->RestoreTPL (OldTpl); return Status;
}
STATIC EFI_STATUS EFIAPI Flush (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This) {
  if (!ValidHost (This)) { return EFI_INVALID_PARAMETER; }
  if (mClosed) { return EFI_DEVICE_ERROR; }
  MemoryFence ();
  // Host posted-write barrier only. Device cache sync is a separate SCSI CDB.
  return MmioRead32 (HCI + 0x34) == 0xFFFFFFFFU ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS Complete (VOID) {
  BOOLEAN DataValid;
  MemoryFence ();
  EFI_STATUS Status = Mt6991UfsValidateResponse (&mPending, mGeometry, &DataValid);
  if ((MmioRead32 (HCI + 0x20) & 0x70884U) || EFI_ERROR (Status)) {
    StopController (); return EFI_DEVICE_ERROR;
  }
  Status = Mt6991UfsDmaComplete (&mDma, DataValid);
  if (!EFI_ERROR (Status) && DataValid && mPending.Transaction == 1 &&
      (mPending.Opcode == 0x35 || mPending.Opcode == 0x91)) {
    if (mCompletedSync[mPending.Lun] == MAX_UINT64) { Status = EFI_DEVICE_ERROR; }
    else {
      ++mCompletedSync[mPending.Lun];
      DEBUG ((DEBUG_WARN, "DALI-UFS-HOST: run=%a SYNC_COMPLETE lun=%u tag=%u opcode=%x count=%Lu\n",
        (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), mPending.Lun, mPending.Tag,
        mPending.Opcode, mCompletedSync[mPending.Lun]));
    }
  }
  if (EFI_ERROR (Status)) { StopController (); }
  return Status;
}
STATIC EFI_STATUS EFIAPI ReadLocked (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Buffer) {
  if (!ValidHost (This) || Buffer == NULL || Width != EfiUfsHcWidthUint32 ||
      Count != 1 || Offset >= WINDOW || (Offset & 3)) { return EFI_INVALID_PARAMETER; }
  if (mClosed) { return EFI_DEVICE_ERROR; }
  UINT32 Value = MmioRead32 (HCI + (UINTN)Offset);
  if (Value == 0xFFFFFFFFU) { StopController (); return EFI_DEVICE_ERROR; }
  if (Offset == 0x58 && mDma.Busy && Value == 0) {
    EFI_STATUS Status = Complete ();
    if (EFI_ERROR (Status)) { return Status; }
  }
  CopyMem (Buffer, &Value, sizeof Value); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI WriteLocked (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Buffer) {
  UINT32 Value; VOID *Cpu;
  if (!ValidHost (This) || Buffer == NULL || Width != EfiUfsHcWidthUint32 ||
      Count != 1 || Offset >= WINDOW || (Offset & 3)) { return EFI_INVALID_PARAMETER; }
  CopyMem (&Value, Buffer, sizeof Value);
  if (Offset == 0x34 && Value == 0) { return StopController (); }
  if (mClosed) { return EFI_DEVICE_ERROR; }
  if (Offset == 0x58) {
    if (Value != 1 || !mTrl || mDma.Busy || MmioRead32 (HCI + 0x58) ||
        MmioRead32 (HCI + 0x78) || MmioRead32 (HCI + 0x60) != 1) { return EFI_ACCESS_DENIED; }
    EFI_STATUS Status = Mt6991UfsValidateTransfer (&mDma, mTrl, mGeometry, &mScratchGrant,
      !mInitUsed && !mInitFinished, &mPending);
    if (!EFI_ERROR (Status)) { Status = Mt6991UfsDmaSubmit (&mDma); }
    if (!EFI_ERROR (Status)) {
      if (mPending.Transaction == 0x16 && mPending.Request[12] == 6) { mInitUsed = TRUE; }
      MmioWrite32 (HCI + 0x20, 1); MemoryFence ();
      MmioWrite32 (HCI + 0x58, 1); MemoryFence ();
    }
    return Status;
  }
  if (Offset == 0x5C) { return StopController (); }
  if (mDma.Busy) { return EFI_NOT_READY; }
  switch (Offset) {
    case 0x50: case 0x70:
      if ((Value & 1023) || EFI_ERROR (Mt6991UfsDmaResolve (&mDma, Value, Offset == 0x50 ? 32 : 80, TRUE, &Cpu))) { return EFI_ACCESS_DENIED; }
      if (Offset == 0x50) { mTrl = Value; } else { mTmrl = Value; }
      break;
    case 0x54: case 0x74: case 0x24:
      if (Value) { return EFI_ACCESS_DENIED; } break;
    case 0x60: case 0x80:
      if (Value > 1 || (Value && !(Offset == 0x60 ? mTrl : mTmrl))) { return EFI_ACCESS_DENIED; } break;
    default: return EFI_ACCESS_DENIED; // Includes task doorbells, UIC and MCQ.
  }
  MmioWrite32 (HCI + (UINTN)Offset, Value); MemoryFence (); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Buffer) {
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = ReadLocked (This, Width, Offset, Count, Buffer);
  gBS->RestoreTPL (OldTpl); return Status;
}
STATIC EFI_STATUS EFIAPI Write (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Buffer) {
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = WriteLocked (This, Width, Offset, Count, Buffer);
  gBS->RestoreTPL (OldTpl); return Status;
}
STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL mHost = { GetBar, Allocate, Free, Map, Unmap, Flush, Read, Write };

STATIC EFI_STATUS EFIAPI Override (EFI_HANDLE Controller, EDKII_UFS_HC_INFO *Info) {
  if (Controller != mController || Info == NULL || mClosed) { return EFI_INVALID_PARAMETER; }
  Info->Capabilities &= ~0x0007001FU; // Exactly one transfer and one task slot.
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Callback (EFI_HANDLE Controller, EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase, VOID *Data) {
  if (Controller != mController) { return EFI_ACCESS_DENIED; }
  if (Phase == EdkiiUfsHcAbort) { return StopController (); }
  if (mClosed) { return EFI_DEVICE_ERROR; }
  if (Phase == EdkiiUfsHcDeviceIdentity) {
    EDKII_UFS_HC_DEVICE_IDENTITY *Identity = Data;
    if (Identity == NULL) { return EFI_INVALID_PARAMETER; }
    EFI_STATUS Status = DaliUfsIdentityCapture (&mIdentity, Identity->DeviceDescriptor,
      Identity->DeviceDescriptorBytes, Identity->SerialDescriptor, Identity->SerialDescriptorBytes);
    // Raw serial bytes never leave the transient query/capture buffers.
    DEBUG ((DEBUG_WARN, "DALI-UFS-IDENTITY: run=%a result=%r ready=%u\n",
      (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), Status, mIdentity.Ready));
    if (Status == EFI_SUCCESS) {
      STATIC CONST CHAR8 HexDigits[] = "0123456789abcdef";
      CHAR8 Hex[SHA256_DIGEST_SIZE * 2 + 1];
      for (UINTN Index = 0; Index < sizeof mIdentity.Digest; ++Index) {
        Hex[Index * 2] = HexDigits[mIdentity.Digest[Index] >> 4];
        Hex[Index * 2 + 1] = HexDigits[mIdentity.Digest[Index] & 15];
      }
      Hex[sizeof Hex - 1] = 0;
      DEBUG ((DEBUG_WARN, "DALI-UFS-IDENTITY: run=%a sha256=%a\n",
        (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), Hex));
    }
    return Status;
  }
  if (Phase == EdkiiUfsHcGetMediaPolicy) {
    if (Data == NULL) { return EFI_INVALID_PARAMETER; }
    EDKII_UFS_HC_MEDIA_POLICY *Policy = Data;
    Policy->ReadOnly = TRUE; Policy->SyncTimeout = COMMAND_TIMEOUT;
    return EFI_SUCCESS;
  }
  if (Phase == EdkiiUfsHcUnitDescriptor) {
    EDKII_UFS_HC_UNIT_DESCRIPTOR_POLICY *Unit = Data;
    if (Unit == NULL || Unit->Lun >= 8) { return EFI_INVALID_PARAMETER; }
    mGeometry[Unit->Lun].WriteProtectionKnown = Unit->Enabled && Unit->WriteProtectionKnown;
    mGeometry[Unit->Lun].WriteProtected = Unit->Enabled && Unit->WriteProtectionKnown && Unit->WriteProtected;
    return EFI_SUCCESS;
  }
  if (Phase == EdkiiUfsHcValidateScsi || Phase == EdkiiUfsHcCompleteScsi) {
    EDKII_UFS_HC_SCSI_POLICY *Policy = Data;
    if (Policy == NULL || Policy->Packet == NULL) { return EFI_INVALID_PARAMETER; }
    EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *P = Policy->Packet;
    if (Phase == EdkiiUfsHcValidateScsi) {
      if (Policy->Event != NULL || P->Cdb == NULL || EFI_ERROR (Mt6991UfsBoundTimeout (&P->Timeout))) { return EFI_ACCESS_DENIED; }
      UINT8 Opcode = ((UINT8 *)P->Cdb)[0];
      if (Opcode == 0x2A || Opcode == 0x8A) {
        if (P->InTransferLength || P->InDataBuffer != NULL ||
            P->DataDirection != EFI_EXT_SCSI_DATA_DIRECTION_WRITE ||
            P->OutTransferLength == 0 || P->OutDataBuffer == NULL) { return EFI_ACCESS_DENIED; }
        return Mt6991UfsScratchWriteCommand (mGeometry, &mScratchGrant, Policy->Lun,
          P->Cdb, P->CdbLength, P->OutTransferLength);
      }
      if (P->OutTransferLength || P->OutDataBuffer != NULL ||
          (P->InTransferLength && (P->DataDirection != EFI_EXT_SCSI_DATA_DIRECTION_READ || P->InDataBuffer == NULL))) { return EFI_ACCESS_DENIED; }
      return Mt6991UfsReadOnlyCommand (mGeometry, Policy->Lun, P->Cdb, P->CdbLength, P->InTransferLength);
    }
    if (P->TargetStatus == 0 && P->Cdb != NULL && P->InDataBuffer != NULL) {
      UINT8 *Bytes = P->InDataBuffer;
      UINT8 *Cdb = P->Cdb;
      // Virtual policy WP flag; the raw UFS UnitDescriptor remains unmodified.
      if (Cdb[0] == 0x1A && P->InTransferLength >= 3) { Bytes[2] |= 0x80; }
      if (Cdb[0] == 0x5A && P->InTransferLength >= 4) { Bytes[3] |= 0x80; }
    }
    return EFI_SUCCESS;
  }
  if (Phase == EdkiiUfsHcValidateQuery) {
    EDKII_UFS_HC_QUERY_POLICY *P = Data;
    if (P == NULL || !P->Timeout || P->Timeout > COMMAND_TIMEOUT ||
        P->DataDirection != (P->Opcode == 6 ? 1 : 2)) { return EFI_ACCESS_DENIED; }
    return Mt6991UfsReadOnlyQuery (P->Opcode, P->Idn, P->Index, P->Selector,
      P->TransferLength, !mInitUsed && !mInitFinished);
  }
  if (Phase == EdkiiUfsHcPostLinkStartup) { mInitFinished = TRUE; }
  return Phase <= EdkiiUfsHcPostLinkStartup ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL mPlatform = {
  EDKII_UFS_HC_PLATFORM_IDENTITY_VERSION, Override, Callback,
  EdkiiUfsCardRefClkFreqObsolete, TRUE, TRUE
};
STATIC struct { VENDOR_DEVICE_PATH Vendor; EFI_DEVICE_PATH_PROTOCOL End; } mPath = {
  { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x8de06254, 0xf7fd, 0x440a, { 0xbb, 0xda, 0x26, 0x99, 0x01, 0x00, 0x00, 0x01 } } },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};
STATIC EFI_STATUS EFIAPI Initialize (CONST VOID *Fdt, EFI_HANDLE *Controller) {
  if (Controller == NULL || !FixedPcdGetBool (PcdDaliUfsReadOnlyEnabled) || FixedPcdGetBool (PcdDaliUfsProbeEnabled)) { return EFI_ACCESS_DENIED; }
  *Controller = NULL;
  if (mAttempted || mShutdown.Attempted) { return EFI_ALREADY_STARTED; }
  mAttempted = TRUE;
  // A known-answer check precedes all controller writes. Build packaging also
  // rejects the null crypto library, whose DEBUG stub would assert here.
  STATIC CONST UINT8 Expected[SHA256_DIGEST_SIZE] = {
    0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
    0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD
  };
  UINT8 Digest[SHA256_DIGEST_SIZE];
  Print (L"UFS: SHA-256 self-test\r\n");
  if (!Sha256HashAll ("abc", 3, Digest) || CompareMem (Digest, Expected, sizeof Digest) != 0) {
    Print (L"UFS: SHA-256 self-test failed; controller untouched.\r\n");
    return EFI_DEVICE_ERROR;
  }
  Print (L"UFS: SHA-256 self-test=Success\r\n");
  Print (L"UFS: controller start run=%a\r\n", (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken));
  MT6991_UFS_IO Io = { IoRead, IoWrite, Fence, Stall, Sip, NULL };
  MT6991_UFS_RESULT Result = Mt6991UfsStart (Fdt, TRUE, FixedPcdGetPtr (PcdDaliUfsStorageToken), &Io, &mControl);
  Print (L"UFS: controller result=%u stage=%u fault=%lx/%x\r\n",
    Result, mControl.Stage, mControl.FaultAddress, mControl.FaultValue);
  DEBUG ((DEBUG_WARN, "DALI-UFS-HOST: run=%a link=%u stage=%u fault=%lx/%x\n",
    (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), Result, mControl.Stage, mControl.FaultAddress, mControl.FaultValue));
  DEBUG ((DEBUG_WARN, "DALI-UFS-CONTROL: run=%a reset low=%u release-attempt=%u released=%u cleanup=%u/%lx sip=%x/%x/%x/%lx\n",
    (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), mControl.DeviceResetAsserted,
    mControl.DeviceResetReleaseAttempted, mControl.DeviceResetReleased, mControl.DeviceResetCleanupAttempted,
    mControl.DeviceResetCleanupStatus, mControl.SipSelector, mControl.SipArg1, mControl.SipArg2, mControl.SipStatus));
  if (mControl.UicFailure.Captured) {
    MT6991_UFS_UIC_SNAPSHOT *S = &mControl.UicFailure;
    DEBUG ((DEBUG_WARN, "DALI-UFS-CONTROL: run=%a first-failure stage=%u opcode=%x is=%x hcs=%x hce=%x arg=%x/%x/%x\n",
      (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), mControl.FaultStage, mControl.UicOpcode,
      S->InterruptStatus, S->HostStatus, S->HostEnable, S->Arguments[0], S->Arguments[1], S->Arguments[2]));
    DEBUG ((DEBUG_WARN, "DALI-UFS-CONTROL: run=%a ue=%x/%x/%x/%x/%x\n",
      (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), S->LayerErrors[0], S->LayerErrors[1],
      S->LayerErrors[2], S->LayerErrors[3], S->LayerErrors[4]));
  }
  if (mControl.LinkResetPending || mControl.LinkResetEvent.Captured) {
    MT6991_UFS_UIC_SNAPSHOT *S = &mControl.LinkResetEvent;
    DEBUG ((DEBUG_WARN, "DALI-UFS-CONTROL: run=%a linereset deferred=%u first-is=%x polls=%u captured=%u accepted=%u acked=%u is=%x hcs=%x hce=%x ue=%x/%x/%x/%x/%x\n",
      (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), mControl.LinkResetPending, mControl.LinkResetFirstIs,
      mControl.LinkResetPolls, mControl.LinkResetEvent.Captured, mControl.LinkResetAccepted, mControl.LinkResetAcked,
      S->InterruptStatus, S->HostStatus, S->HostEnable, S->LayerErrors[0], S->LayerErrors[1],
      S->LayerErrors[2], S->LayerErrors[3], S->LayerErrors[4]));
  }
  if (Result != Mt6991UfsLinkReady) { return EFI_DEVICE_ERROR; }
  Print (L"UFS: DMA initialize\r\n");
  EFI_STATUS Status = Mt6991UfsDmaUefiInit (&mDma, 0xFFFFFFFFULL);
  Print (L"UFS: DMA result=%r\r\n", Status);
  if (EFI_ERROR (Status)) { StopController (); return Status; }
  mClosed = FALSE;
  Print (L"UFS: install host protocols\r\n");
  Status = gBS->InstallMultipleProtocolInterfaces (&mController,
    &gEdkiiUfsHostControllerProtocolGuid, &mHost, &gEdkiiUfsHcPlatformProtocolGuid, &mPlatform,
    &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (!EFI_ERROR (Status)) {
    Print (L"UFS: connect PassThru / SCSI / BlockIo\r\n");
    Status = gBS->ConnectController (mController, NULL, NULL, TRUE);
  }
  if (Status != EFI_SUCCESS && !EFI_ERROR (Status)) { Status = EFI_DEVICE_ERROR; }
  if (Status == EFI_SUCCESS && !mIdentity.Ready) { Status = EFI_SECURITY_VIOLATION; }
  Print (L"UFS: connect result=%r\r\n", Status);
  *Controller = mController;
  DEBUG ((DEBUG_WARN, "DALI-UFS-HOST: run=%a connect=%r\n", (CHAR8 *)FixedPcdGetPtr (PcdDaliUfsStorageToken), Status));
  if (Status == EFI_SUCCESS) {
    if (!FixedPcdGetBool (PcdDaliUfsBootStageEnabled)) {
      Print (L"UFS: read-only BlockIo diagnostics\r\n");
      Status = DaliUfsReadOnlyDiagnostics (mController, FixedPcdGetPtr (PcdDaliUfsStorageToken), mCompletedSync);
      Print (L"UFS: BlockIo result=%r\r\n", Status);
    }
  }
  if (Status == EFI_SUCCESS) {
    Status = DaliUfsScratchPublish (mController, &mScratchGrant, mGeometry, &mClosed, mCompletedSync);
    if (Status == EFI_NOT_STARTED) { Status = EFI_SUCCESS; }
  }
  if (EFI_ERROR (Status)) { StopController (); }
  return Status;
}
STATIC EFI_STATUS EFIAPI PublishBootStage (VOID) {
  if (!FixedPcdGetBool (PcdDaliUfsBootStageEnabled) || mClosed || mController == NULL) { return EFI_ACCESS_DENIED; }
  return DaliUfsBootStagePublish (mController);
}
STATIC DALI_UFS_DIAGNOSTIC_PROTOCOL mDiagnostic = { Initialize, PublishBootStage, Quiesce };
STATIC UNIXNU_STORAGE_QUIESCE_PROTOCOL mStorage = { UNIXNU_STORAGE_QUIESCE_REVISION, StorageQuiesce };
EFI_STATUS EFIAPI DaliUfsHostEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)SystemTable;
  if (!FixedPcdGetBool (PcdDaliUfsReadOnlyEnabled)) { return EFI_NOT_STARTED; }
  if (FixedPcdGetBool (PcdDaliUfsProbeEnabled)) { return EFI_ACCESS_DENIED; }
  return gBS->InstallMultipleProtocolInterfaces (&Image,
    &gDaliUfsDiagnosticProtocolGuid, &mDiagnostic, &gUniXNUStorageQuiesceProtocolGuid, &mStorage, NULL);
}
