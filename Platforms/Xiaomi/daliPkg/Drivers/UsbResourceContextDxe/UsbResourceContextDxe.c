// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Protocol/DaliUsbResourceContext.h>
#include <Guid/EventGroup.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DaliUsbPmicLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <DaliUsbVusb.h>

#define PAGE_BYTES  4096U
#define MAX_PAGES   (DaliUsbClockBankCount * 2 + 5)
STATIC struct {
  DALI_USB_RESOURCE_CONTEXT_STATUS Status;
  DALI_USB_CLOCK_RESOURCES Clocks;
  DALI_USB_XHCI_RESOURCES Xhci;
  VOID *Snapshot;
  UINTN SnapshotSize;
  EFI_HANDLE Image;
  EFI_PHYSICAL_ADDRESS Pages[MAX_PAGES];
  UINTN PageCount;
  BOOLEAN Entered;
} mContext;
STATIC struct {
  DALI_USB_VUSB_RESOURCES Resources;
  DALI_USB_PMIC Pmic;
  struct dali_usb_vusb Rail;
  DALI_USB_VUSB_TOKEN *Owner;
  DALI_USB_VUSB_GUARD Guard;
  VOID *GuardContext;
  volatile UINT32 Busy;
} mVusb;
STATIC DALI_USB_I2C_RESOURCES mI2c;
STATIC DALI_USB_NATIVE_I2C mI2cBus;
STATIC DALI_USB_I2C_TOKEN *mI2cOwner;

STATIC EFI_STATUS EFIAPI ReadStatus (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_RESOURCE_CONTEXT_STATUS *Out);
STATIC EFI_STATUS EFIAPI ReadClocks (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_CLOCK_RESOURCES *Out);
STATIC EFI_STATUS EFIAPI ReadVusb (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_VUSB_RESOURCES *Out);
STATIC EFI_STATUS EFIAPI AcquireVusb (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_VUSB_TOKEN *, DALI_USB_VUSB_GUARD, VOID *, DALI_USB_VUSB_INFO *);
STATIC EFI_STATUS EFIAPI ReleaseVusb (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_VUSB_TOKEN *);
STATIC EFI_STATUS EFIAPI ReadI2c (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_RESOURCES *);
STATIC EFI_STATUS EFIAPI ReadXhci (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_XHCI_RESOURCES *);
STATIC EFI_STATUS EFIAPI ReadPins (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_RESOURCE_GUARD, VOID *, struct dali_i2c6_pin_snapshot *);
STATIC EFI_STATUS EFIAPI AcquireBus (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_TOKEN *, DALI_USB_I2C_POLICY, VOID *, UINT32, struct dali_nxp_repeater_bus *, DALI_USB_I2C_INFO *);
STATIC EFI_STATUS EFIAPI BusStatus (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_TOKEN *, DALI_USB_I2C_INFO *);
STATIC EFI_STATUS EFIAPI ReleaseBus (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_TOKEN *);
STATIC DALI_USB_RESOURCE_CONTEXT_PROTOCOL mProtocol = {6, ReadStatus, ReadClocks, ReadVusb, AcquireVusb, ReleaseVusb,
  ReadI2c, ReadPins, AcquireBus, BusStatus, ReleaseBus, ReadXhci};

STATIC EFI_STATUS EFIAPI ReadStatus (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_RESOURCE_CONTEXT_STATUS *Out) {
  if (This != &mProtocol || Out == NULL) { return EFI_INVALID_PARAMETER; }
  if (mContext.Status.BootServicesClosed) { return EFI_UNSUPPORTED; }
  *Out = mContext.Status; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReadClocks (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_CLOCK_RESOURCES *Out) {
  if (This != &mProtocol || Out == NULL) { return EFI_INVALID_PARAMETER; }
  if (mContext.Status.BootServicesClosed) { return EFI_UNSUPPORTED; }
  if (EFI_ERROR (mContext.Status.InitializationStatus)) { return mContext.Status.InitializationStatus; }
  *Out = mContext.Clocks; return EFI_SUCCESS;
}
STATIC VOID EFIAPI ExitBootServices (EFI_EVENT Event, VOID *Context) {
  (VOID)Event; (VOID)Context;
  mVusb.Pmic.Faulted = TRUE; // Block every new PMIF transfer, independent of event order.
  mI2cBus.Closed = TRUE; mI2cBus.Faulted = TRUE; mI2cBus.Ready = FALSE;
  mContext.Status.BootServicesClosed = TRUE;
}
STATIC EFI_STATUS EFIAPI ReadVusb (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_VUSB_RESOURCES *Out) {
  if (This != &mProtocol || Out == NULL) { return EFI_INVALID_PARAMETER; }
  if (mContext.Status.BootServicesClosed) { return EFI_UNSUPPORTED; }
  if (EFI_ERROR (mContext.Status.InitializationStatus)) { return mContext.Status.InitializationStatus; }
  *Out = mVusb.Resources; return EFI_SUCCESS;
}
STATIC EFI_STATUS EfiResult (int Result) {
  switch (Result) {
    case USB_OK: return EFI_SUCCESS;
    case USB_BUSY: return EFI_NOT_READY;
    case USB_UNSUPPORTED: return EFI_UNSUPPORTED;
    case USB_TIMEOUT: return EFI_TIMEOUT;
    case USB_INVALID: case USB_RANGE: return EFI_INVALID_PARAMETER;
    default: return EFI_DEVICE_ERROR;
  }
}
STATIC int PowerGuard (VOID *Context) {
  (VOID)Context;
  if (mContext.Status.BootServicesClosed) { return USB_UNSUPPORTED; }
  EFI_STATUS Status = mVusb.Guard (mVusb.GuardContext);
  if (mContext.Status.BootServicesClosed) { return USB_UNSUPPORTED; }
  return !EFI_ERROR (Status) ? USB_OK : Status == EFI_NOT_READY ? USB_BUSY :
    Status == EFI_UNSUPPORTED ? USB_UNSUPPORTED : Status == EFI_TIMEOUT ? USB_TIMEOUT : USB_IO;
}
STATIC EFI_STATUS PowerEnter (VOID) {
  if (mContext.Status.BootServicesClosed) { return EFI_UNSUPPORTED; }
  if (EFI_ERROR (mContext.Status.InitializationStatus)) { return mContext.Status.InitializationStatus; }
  EFI_TPL Old = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (Old != TPL_APPLICATION) { gBS->RestoreTPL (Old); return EFI_UNSUPPORTED; }
  if (InterlockedCompareExchange32 (&mVusb.Busy, 0, 1) != 0) { gBS->RestoreTPL (Old); return EFI_NOT_READY; }
  gBS->RestoreTPL (Old);
  if (mContext.Status.BootServicesClosed) { InterlockedCompareExchange32 (&mVusb.Busy, 1, 0); return EFI_UNSUPPORTED; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PowerLeave (EFI_STATUS Status) {
  InterlockedCompareExchange32 (&mVusb.Busy, 1, 0); return Status;
}
STATIC int UsbResult(EFI_STATUS Status) {
  return !EFI_ERROR(Status) ? USB_OK : Status == EFI_NOT_READY ? USB_BUSY :
    Status == EFI_UNSUPPORTED ? USB_UNSUPPORTED : Status == EFI_TIMEOUT ? USB_TIMEOUT :
    Status == EFI_INVALID_PARAMETER ? USB_INVALID : USB_IO;
}
STATIC EFI_STATUS EFIAPI AcquireVusb (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_VUSB_TOKEN *Token,
  DALI_USB_VUSB_GUARD Guard, VOID *Context, DALI_USB_VUSB_INFO *Out) {
  if (This != &mProtocol || Token == NULL || Guard == NULL || Out == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = PowerEnter ();
  if (EFI_ERROR (Status)) { return Status; }
  if (mVusb.Owner != NULL && mVusb.Owner != Token) { return PowerLeave (EFI_NOT_READY); }
  if (mVusb.Owner == NULL) {
    if (Token->Active) { return PowerLeave (EFI_INVALID_PARAMETER); }
    mVusb.Owner = Token; Token->Active = TRUE; mVusb.Guard = Guard; mVusb.GuardContext = Context;
    int Result = PowerGuard (NULL);
    Status = EfiResult (Result);
    if (!EFI_ERROR (Status)) { Status = DaliUsbVusbPmicOpen (&mVusb.Pmic, &mVusb.Resources); }
    if (!EFI_ERROR (Status)) {
      struct dali_repeater_guard G = {NULL, PowerGuard};
      Status = EfiResult (dali_usb_vusb_init (&mVusb.Rail, &mVusb.Pmic.Transport, &G, true));
    }
  } else if (mVusb.Guard != Guard || mVusb.GuardContext != Context || !Token->Active) {
    return PowerLeave (EFI_ACCESS_DENIED);
  } else if (!mVusb.Rail.initialized) { return PowerLeave (EFI_DEVICE_ERROR); }
  if (!EFI_ERROR (Status)) { Status = EfiResult (dali_usb_vusb_acquire (&mVusb.Rail)); }
  if (mContext.Status.BootServicesClosed) { Status = EFI_UNSUPPORTED; }
  if (!EFI_ERROR (Status)) {
    DALI_USB_VUSB_INFO Value = {mVusb.Rail.nominal_uv, mVusb.Rail.selector,
      mVusb.Rail.trim, mVusb.Rail.lp, mVusb.Pmic.Transport.revision};
    *Out = Value;
  }
  return PowerLeave (Status);
}
STATIC EFI_STATUS EFIAPI ReleaseVusb (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_VUSB_TOKEN *Token) {
  if (This != &mProtocol || Token == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = PowerEnter ();
  if (EFI_ERROR (Status)) { return Status; }
  if (mVusb.Owner != Token || !Token->Active) { return PowerLeave (EFI_ACCESS_DENIED); }
  Status = EfiResult (PowerGuard (NULL));
  if (!EFI_ERROR (Status) && mVusb.Rail.initialized) { Status = EfiResult (dali_usb_vusb_release (&mVusb.Rail)); }
  if (!EFI_ERROR (Status)) { Status = DaliUsbPmicClose (&mVusb.Pmic); }
  if (mContext.Status.BootServicesClosed) { Status = EFI_UNSUPPORTED; }
  if (!EFI_ERROR (Status)) {
    Token->Active = FALSE; mVusb.Owner = NULL; mVusb.Guard = NULL; mVusb.GuardContext = NULL;
  }
  return PowerLeave (Status);
}
STATIC EFI_STATUS EFIAPI ReadI2c (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_I2C_RESOURCES *Out) {
  if (This != &mProtocol || Out == NULL) { return EFI_INVALID_PARAMETER; }
  if (mContext.Status.BootServicesClosed) { return EFI_UNSUPPORTED; }
  if (EFI_ERROR (mContext.Status.InitializationStatus)) { return mContext.Status.InitializationStatus; }
  *Out = mI2c; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReadXhci (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_XHCI_RESOURCES *Out) {
  if (This != &mProtocol || Out == NULL) { return EFI_INVALID_PARAMETER; }
  if (mContext.Status.BootServicesClosed) { return EFI_UNSUPPORTED; }
  if (EFI_ERROR (mContext.Status.InitializationStatus)) { return mContext.Status.InitializationStatus; }
  *Out = mContext.Xhci; return EFI_SUCCESS;
}
STATIC int PinRead32 (VOID *Context, UINT32 Offset, UINT32 *Out) {
  CONST DALI_USB_MMIO_RANGE *Range = Context;
  if (mContext.Status.BootServicesClosed) { return USB_UNSUPPORTED; }
  if ((Offset & 3) || Range->Size < 4 || Offset > PAGE_BYTES - 4 || Offset > Range->Size - 4) { return USB_RANGE; }
  MemoryFence ();
  UINT32 Value = MmioRead32 ((UINTN)Range->Base + Offset);
  MemoryFence ();
  if (mContext.Status.BootServicesClosed) { return USB_UNSUPPORTED; }
  *Out = Value; return USB_OK;
}
STATIC VOID PinBarrier (VOID *Context) { (VOID)Context; MemoryFence (); }
STATIC EFI_STATUS PinGuard (DALI_USB_RESOURCE_GUARD Guard, VOID *Context) {
  EFI_STATUS Status = Guard (Context);
  return mContext.Status.BootServicesClosed ? EFI_UNSUPPORTED : Status;
}
STATIC EFI_STATUS EFIAPI ReadPins (DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This,
  DALI_USB_RESOURCE_GUARD Guard, VOID *Context, struct dali_i2c6_pin_snapshot *Out) {
  if (This != &mProtocol || Guard == NULL || Out == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = PowerEnter ();
  if (EFI_ERROR (Status)) { return Status; }
  Status = PinGuard (Guard, Context);
  struct dali_i2c6_pin_snapshot Value;
  if (!EFI_ERROR (Status)) {
    struct usb_mmio Gpio = {&mI2c.Gpio, PAGE_BYTES, PinRead32, NULL, PinBarrier, NULL};
    struct usb_mmio Iocfg = {&mI2c.IocfgLb2, PAGE_BYTES, PinRead32, NULL, PinBarrier, NULL};
    Status = EfiResult (dali_usb_i2c6_pins_read (&Gpio, &Iocfg, &Value));
  }
  if (!EFI_ERROR (Status)) { Status = PinGuard (Guard, Context); }
  if (mContext.Status.BootServicesClosed) { Status = EFI_UNSUPPORTED; }
  if (!EFI_ERROR (Status)) { *Out = Value; }
  return PowerLeave (Status);
}
STATIC int EnterBus(VOID *Context) {
  EFI_STATUS Status = PowerEnter();
  if (EFI_ERROR(Status)) { return UsbResult(Status); }
  DALI_USB_I2C_TOKEN *Token = Context;
  if (Token == NULL || mI2cOwner != Token || !Token->Active) { PowerLeave(EFI_ACCESS_DENIED); return USB_UNSUPPORTED; }
  return USB_OK;
}
STATIC int LeaveBus(int Result) {
  if (mContext.Status.BootServicesClosed) { Result = USB_UNSUPPORTED; }
  PowerLeave(EFI_SUCCESS); return Result;
}
STATIC int BusRead(VOID *Context, UINT8 Reg, UINT8 *Out) {
  if (Out == NULL) { return USB_INVALID; }
  int Result = EnterBus(Context); UINT8 Value;
  if (Result) { return Result; }
  Result = DaliUsbNativeI2cRead(&mI2cBus,Reg,&Value);
  if (mContext.Status.BootServicesClosed) { Result = USB_UNSUPPORTED; }
  if (!Result) { *Out = Value; }
  return LeaveBus(Result);
}
STATIC int BusWrite(VOID *Context, UINT8 Reg, UINT8 Value) {
  int Result = EnterBus(Context);
  if (!Result) { Result = LeaveBus(DaliUsbNativeI2cWrite(&mI2cBus,Reg,Value)); }
  return Result;
}
STATIC int BusRecover(VOID *Context) {
  int Result = EnterBus(Context);
  if (!Result) { Result = LeaveBus(DaliUsbNativeI2cRecover(&mI2cBus)); }
  return Result;
}
STATIC int BusReset(VOID *Context, bool Enable) {
  int Result = EnterBus(Context);
  if (!Result) { Result = LeaveBus(DaliUsbNativeI2cReset(&mI2cBus,Enable)); }
  return Result;
}
STATIC VOID BusDelay(VOID *Context, UINT32 Us) {
  int Result = EnterBus(Context);
  if (Result) {
    // Void bus delays cannot report refusal. Latch a fault so a subsequent
    // peripheral access cannot accept a sequence without its required delay.
    if (mI2cOwner == Context && mI2cBus.Initialized) {
      mI2cBus.Faulted = TRUE; mI2cBus.Ready = FALSE;
    }
    return;
  }
  if (Us > 10000) { mI2cBus.Faulted = TRUE; mI2cBus.Ready = FALSE; }
  else { DaliUsbNativeI2cDelay(&mI2cBus,Us); }
  LeaveBus(USB_OK);
}
STATIC EFI_STATUS EFIAPI AcquireBus(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_I2C_TOKEN *Token,
  DALI_USB_I2C_POLICY Guard, VOID *Context, UINT32 UlposcHz, struct dali_nxp_repeater_bus *Out, DALI_USB_I2C_INFO *Info) {
  if (This != &mProtocol || Token == NULL || Guard == NULL || Out == NULL || Info == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = PowerEnter();
  if (EFI_ERROR(Status)) { return Status; }
  if (mI2cOwner != NULL && mI2cOwner != Token) { return PowerLeave(EFI_NOT_READY); }
  int Result;
  if (mI2cOwner == NULL) {
    if (Token->Active) { return PowerLeave(EFI_INVALID_PARAMETER); }
    mI2cOwner = Token; Token->Active = TRUE;
    Result = DaliUsbNativeI2cOpen(&mI2cBus,&mContext.Clocks,&mI2c,Guard,Context,UlposcHz);
  } else if (!Token->Active || mI2cBus.Policy != Guard || mI2cBus.PolicyContext != Context || mI2cBus.UlposcHz != UlposcHz) {
    return PowerLeave(EFI_ACCESS_DENIED);
  } else { Result = DaliUsbNativeI2cCheck(&mI2cBus); }
  DALI_USB_I2C_INFO Value;
  if (!Result) { Result = DaliUsbNativeI2cInfo(&mI2cBus,&Value); }
  if (mContext.Status.BootServicesClosed) { Result = USB_UNSUPPORTED; }
  if (!Result) {
    struct dali_nxp_repeater_bus Bus = {Token,BusRead,BusWrite,BusRecover,BusReset,BusDelay};
    *Out = Bus; *Info = Value;
  }
  return PowerLeave(EfiResult(Result));
}
STATIC EFI_STATUS EFIAPI BusStatus(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_I2C_TOKEN *Token, DALI_USB_I2C_INFO *Out) {
  if (This != &mProtocol || Token == NULL || Out == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = PowerEnter();
  if (EFI_ERROR(Status)) { return Status; }
  if (mI2cOwner != Token || !Token->Active) { return PowerLeave(EFI_ACCESS_DENIED); }
  return PowerLeave(EfiResult(DaliUsbNativeI2cInfo(&mI2cBus,Out)));
}
STATIC EFI_STATUS EFIAPI ReleaseBus(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This, DALI_USB_I2C_TOKEN *Token) {
  if (This != &mProtocol || Token == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = PowerEnter();
  if (EFI_ERROR(Status)) { return Status; }
  if (mI2cOwner != Token || !Token->Active) { return PowerLeave(EFI_ACCESS_DENIED); }
  Status = EfiResult(DaliUsbNativeI2cClose(&mI2cBus));
  if (mContext.Status.BootServicesClosed) { Status = EFI_UNSUPPORTED; }
  if (!EFI_ERROR(Status)) { Token->Active = FALSE; mI2cOwner = NULL; }
  return PowerLeave(Status);
}

STATIC BOOLEAN Covers (EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D, EFI_PHYSICAL_ADDRESS Page) {
  return D->BaseAddress <= Page && D->Length >= PAGE_BYTES && Page - D->BaseAddress <= D->Length - PAGE_BYTES;
}
STATIC EFI_STATUS MapPage (EFI_PHYSICAL_ADDRESS Page) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS Status = gDS->GetMemorySpaceDescriptor (Page, &D);
  if (EFI_ERROR (Status)) { return Status; }
  if (!Covers (&D, Page) || D.ImageHandle != NULL || D.DeviceHandle != NULL) { return EFI_ACCESS_DENIED; }
  if (D.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    if (D.Attributes != 0) { return EFI_UNSUPPORTED; }
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Page, PAGE_BYTES, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) { return Status; }
    Status = gDS->GetMemorySpaceDescriptor (Page, &D);
    if (EFI_ERROR (Status)) { return Status; }
  }
  if (!Covers (&D, Page) || D.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo ||
      D.ImageHandle != NULL || D.DeviceHandle != NULL || !(D.Capabilities & EFI_MEMORY_UC) ||
      (D.Attributes != 0 && D.Attributes != EFI_MEMORY_UC && D.Attributes != (EFI_MEMORY_UC | EFI_MEMORY_XP))) { return EFI_ACCESS_DENIED; }
  EFI_PHYSICAL_ADDRESS Address = Page;
  Status = gDS->AllocateMemorySpace (EfiGcdAllocateAddress, EfiGcdMemoryTypeMemoryMappedIo, 12, PAGE_BYTES, &Address, mContext.Image, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  if (Address != Page) { return EFI_COMPROMISED_DATA; }
  UINT64 Attributes = EFI_MEMORY_UC | (D.Attributes & EFI_MEMORY_XP);
  // Let CpuDxe observe the old CPU attributes and perform required cache
  // maintenance before the direct MMU call could hide a cacheable mapping.
  Status = gDS->SetMemorySpaceAttributes (Page, PAGE_BYTES, Attributes);
  if (EFI_ERROR (Status)) { return Status; }
  // Force the real ARM mapping independently of metadata-only GCD sync. This
  // DXE entry runs at APPLICATION after CpuDxe's synchronous init/SyncCacheConfig.
  // One page bounds partial failures. Device UC implies XN in the pinned MMU.
  Status = ArmSetMemoryAttributes (Page, PAGE_BYTES, Attributes, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gDS->GetMemorySpaceDescriptor (Page, &D);
  if (EFI_ERROR (Status)) { return Status; }
  if (!Covers (&D, Page) || D.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo ||
      D.ImageHandle != mContext.Image || D.DeviceHandle != NULL || D.Attributes != Attributes) { return EFI_DEVICE_ERROR; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Prepare (CONST VOID *Source, UINTN Capacity) {
  if (Capacity < 40 || FdtCheckHeader (Source) != 0) { return EFI_COMPROMISED_DATA; }
  UINTN Size = FdtTotalSize (Source);
  if (Size < 40 || Size > Capacity) { return EFI_COMPROMISED_DATA; }
  mContext.Snapshot = AllocatePool (Size);
  if (mContext.Snapshot == NULL) { return EFI_OUT_OF_RESOURCES; }
  mContext.SnapshotSize = Size; CopyMem (mContext.Snapshot, Source, Size);
  DALI_USB_CLOCK_RESOURCES Clocks;
  EFI_STATUS Status = DaliUsbReadClockResources (mContext.Snapshot, Size, &Clocks);
  if (EFI_ERROR (Status)) { return Status; }
  DALI_USB_XHCI_RESOURCES Xhci;
  Status = DaliUsbReadXhciResources (mContext.Snapshot, Size, &Xhci);
  if (EFI_ERROR (Status)) { return Status; }
  Status = DaliUsbReadVusbResources (mContext.Snapshot, Size, &mVusb.Resources);
  if (EFI_ERROR (Status)) { return Status; }
  Status = DaliUsbReadI2cResources (mContext.Snapshot, Size, &mI2c);
  if (EFI_ERROR (Status)) { return Status; }
  // The fixed SEC arena is stable during this BSP DXE entry. Also reject any
  // observed change while copying. Subsequent source changes cannot modify our
  // private snapshot; no API returns its pointer, and no code writes it again.
  if (CompareMem (mContext.Snapshot, Source, Size) != 0) { return EFI_COMPROMISED_DATA; }
  mContext.Status.SnapshotValid = TRUE; mContext.Clocks = Clocks; mContext.Xhci = Xhci;
  DALI_USB_MMIO_RANGE Ranges[DaliUsbClockBankCount + 5];
  for (UINTN I = 0; I < DaliUsbClockBankCount; I++) { Ranges[I] = Clocks.Bank[I]; }
  // GPIO already has an initial platform mapping; claim/recheck it with the
  // same GCD/CPU rules. The remaining three pages are absent from that map.
  Ranges[DaliUsbClockBankCount] = mI2c.Gpio;
  Ranges[DaliUsbClockBankCount + 1] = mI2c.IocfgLb2;
  Ranges[DaliUsbClockBankCount + 2] = (DALI_USB_MMIO_RANGE){mI2c.Controller.Base, PAGE_BYTES};
  Ranges[DaliUsbClockBankCount + 3] = (DALI_USB_MMIO_RANGE){mI2c.Dma.Base, PAGE_BYTES};
  Ranges[DaliUsbClockBankCount + 4] = Xhci.Registers;
  for (UINTN I = 0; I < DaliUsbClockBankCount + 5; I++) {
    UINT64 End = (UINT64)Ranges[I].Base + Ranges[I].Size;
    if (Ranges[I].Size == 0 || End > 0x100000000ULL) { return EFI_COMPROMISED_DATA; }
    for (EFI_PHYSICAL_ADDRESS Page = Ranges[I].Base & ~(EFI_PHYSICAL_ADDRESS)(PAGE_BYTES - 1); Page < End; Page += PAGE_BYTES) {
      BOOLEAN Seen = FALSE;
      for (UINTN J = 0; J < mContext.PageCount; J++) { Seen |= mContext.Pages[J] == Page; }
      if (Seen) { continue; }
      if (mContext.PageCount == MAX_PAGES) { return EFI_OUT_OF_RESOURCES; }
      mContext.Pages[mContext.PageCount++] = Page;
    }
  }
  // TOP/APMIXED share a page. All pages are deduplicated before any mutation.
  for (UINTN I = 0; I < mContext.PageCount; I++) {
    mContext.Status.MappingAttempts++;
    Status = MapPage (mContext.Pages[I]);
    if (EFI_ERROR (Status)) { return Status; }
    mContext.Status.MappedPages++;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI DaliUsbResourceContextInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)SystemTable;
  if (mContext.Entered) { return EFI_ALREADY_STARTED; }
  mContext.Entered = TRUE; mContext.Image = Image; mContext.Status.InitializationStatus = EFI_NOT_STARTED;
  EFI_TPL Previous = gBS->RaiseTPL (TPL_HIGH_LEVEL); gBS->RestoreTPL (Previous);
  if (Previous == TPL_APPLICATION && Image != NULL) {
    mContext.Status.InitializationStatus = Prepare ((CONST VOID *)(UINTN)DALI_FDT_BASE, DALI_FDT_SIZE);
  } else {
    mContext.Status.InitializationStatus = EFI_UNSUPPORTED;
    return EFI_SUCCESS; // No Boot Services publication from an unsupported TPL.
  }
  EFI_EVENT Event;
  EFI_STATUS Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, NULL, &gEfiEventExitBootServicesGuid, &Event);
  if (EFI_ERROR (Status)) {
    mContext.Status.InitializationStatus = Status;
    return EFI_SUCCESS; // Without an EBS event, never expose live accessors.
  }
  EFI_HANDLE Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&Handle, &gDaliUsbResourceContextProtocolGuid, &mProtocol, NULL);
  if (EFI_ERROR (Status)) { mContext.Status.InitializationStatus = Status; }
  DEBUG ((DEBUG_WARN, "DALI-USB: context init=%r publish=%r pages=%u/%u snapshot=%u; no power operation at entry, no source/DMA/controller qualification\n",
    mContext.Status.InitializationStatus, Status, mContext.Status.MappedPages, mContext.Status.MappingAttempts, mContext.Status.SnapshotValid));
  // Preserve this resident context and all mapping attempts even on failure.
  // Unloading or freeing GCD descriptors would not remove live ARM page tables.
  // Publication failure is fail-closed and logged; returning an error could
  // unload the image while the EBS callback and owned mappings still refer to it.
  return EFI_SUCCESS;
}
