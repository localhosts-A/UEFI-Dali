// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/DaliUsbCdcLogLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DaliUsbCdcLog.h>
#include <Library/DaliConsole.h>

#include <assert.h>
#include <stdarg.h>

#define CONSOLE_CAPACITY 64U

UINT8 gConsole[12 + CONSOLE_CAPACITY];
EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_GUID gDaliUsbCdcLogProtocolGuid = {
  0xf4c3670d, 0xb976, 0x43a2, {0xb9, 0x2e, 0x73, 0x9e, 0x25, 0xc5, 0x0a, 0x47}
};
EFI_GUID gEfiEventExitBootServicesGuid = {
  0x27abf055, 0xb1b8, 0x4c26, {0x80, 0x48, 0x74, 0x8f, 0x37, 0xba, 0xa2, 0xdf}
};

typedef struct {
  EFI_EVENT Handle;
  EFI_EVENT_NOTIFY Notify;
  VOID *Context;
  EFI_TPL Tpl;
} MOCK_EVENT;

static EFI_BOOT_SERVICES mBootServices;
static MOCK_EVENT mEvents[8];
static UINTN mEventCount;
static EFI_EVENT mProtocolNotifyEvent;
static VOID *mRegistration;
static DALI_USB_CDC_LOG_PROTOCOL *mInstalledProtocol;
static EFI_TPL mCurrentTpl = TPL_APPLICATION;
static UINTN mBootServiceCalls;
static BOOLEAN mInterruptsEnabled = TRUE;

EFI_STATUS EFIAPI RamoopsCdcSerialPortLibConstructor (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
EFI_STATUS EFIAPI DaliUsbCdcLogInitialize (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

static VOID *HostMemSet (VOID *Destination, UINT8 Value, UINTN Length) {
  UINT8 *Bytes = Destination;
  for (UINTN I = 0; I < Length; I++) { Bytes[I] = Value; }
  return Destination;
}

static int HostMemCmp (CONST VOID *Left, CONST VOID *Right, UINTN Length) {
  CONST UINT8 *A = Left;
  CONST UINT8 *B = Right;
  for (UINTN I = 0; I < Length; I++) {
    if (A[I] != B[I]) { return A[I] < B[I] ? -1 : 1; }
  }
  return 0;
}

static MOCK_EVENT *FindEvent (EFI_EVENT Handle) {
  for (UINTN I = 0; I < mEventCount; I++) {
    if (mEvents[I].Handle == Handle) { return &mEvents[I]; }
  }
  return NULL;
}

static EFI_STATUS EFIAPI MockCreateEvent (UINT32 Type, EFI_TPL NotifyTpl,
  EFI_EVENT_NOTIFY NotifyFunction, VOID *NotifyContext, EFI_EVENT *Event) {
  (VOID)Type;
  assert(mEventCount < sizeof(mEvents) / sizeof(mEvents[0]));
  MOCK_EVENT *Created = &mEvents[mEventCount];
  Created->Handle = (EFI_EVENT)(UINTN)(mEventCount + 1);
  Created->Notify = NotifyFunction;
  Created->Context = NotifyContext;
  Created->Tpl = NotifyTpl;
  *Event = Created->Handle;
  mEventCount++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockCreateEventEx (UINT32 Type, EFI_TPL NotifyTpl,
  EFI_EVENT_NOTIFY NotifyFunction, CONST VOID *NotifyContext,
  CONST EFI_GUID *EventGroup, EFI_EVENT *Event) {
  (VOID)EventGroup;
  return MockCreateEvent(Type, NotifyTpl, NotifyFunction, (VOID *)NotifyContext, Event);
}

static EFI_STATUS EFIAPI MockCloseEvent (EFI_EVENT Event) {
  assert(FindEvent(Event) != NULL);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockRegisterProtocolNotify (EFI_GUID *Protocol, EFI_EVENT Event,
  VOID **Registration) {
  assert(HostMemCmp(Protocol, &gDaliUsbCdcLogProtocolGuid, sizeof(*Protocol)) == 0);
  assert(FindEvent(Event) != NULL);
  mProtocolNotifyEvent = Event;
  mRegistration = (VOID *)(UINTN)0x1234;
  *Registration = mRegistration;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockLocateProtocol (EFI_GUID *Protocol, VOID *Registration,
  VOID **Interface) {
  (VOID)Registration;
  assert(HostMemCmp(Protocol, &gDaliUsbCdcLogProtocolGuid, sizeof(*Protocol)) == 0);
  if (mInstalledProtocol == NULL) { return EFI_NOT_FOUND; }
  *Interface = mInstalledProtocol;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockInstallMultipleProtocolInterfaces (EFI_HANDLE *Handle, ...) {
  va_list Args;
  va_start(Args, Handle);
  EFI_GUID *Protocol = va_arg(Args, EFI_GUID *);
  VOID *Interface = va_arg(Args, VOID *);
  va_end(Args);
  assert(HostMemCmp(Protocol, &gDaliUsbCdcLogProtocolGuid, sizeof(*Protocol)) == 0);
  assert(mInstalledProtocol == NULL);
  *Handle = (EFI_HANDLE)(UINTN)0x9876;
  mInstalledProtocol = (DALI_USB_CDC_LOG_PROTOCOL *)Interface;

  MOCK_EVENT *Notify = FindEvent(mProtocolNotifyEvent);
  assert(Notify != NULL && Notify->Notify != NULL);
  EFI_TPL SavedTpl = mCurrentTpl;
  mCurrentTpl = Notify->Tpl;
  Notify->Notify(Notify->Handle, Notify->Context);
  mCurrentTpl = SavedTpl;
  return EFI_SUCCESS;
}

static EFI_TPL EFIAPI MockRaiseTpl (EFI_TPL NewTpl) {
  mBootServiceCalls++;
  EFI_TPL OldTpl = mCurrentTpl;
  assert(NewTpl >= OldTpl);
  mCurrentTpl = NewTpl;
  return OldTpl;
}

static VOID EFIAPI MockRestoreTpl (EFI_TPL OldTpl) {
  mBootServiceCalls++;
  mCurrentTpl = OldTpl;
}

BOOLEAN ArmGetInterruptState (VOID) { return mInterruptsEnabled; }
VOID ArmDisableInterrupts (VOID) { mInterruptsEnabled = FALSE; }
VOID ArmEnableInterrupts (VOID) { mInterruptsEnabled = TRUE; }
VOID EFIAPI MemoryFence (VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static VOID SignalEvent (EFI_EVENT Handle) {
  MOCK_EVENT *Event = FindEvent(Handle);
  assert(Event != NULL && Event->Notify != NULL);
  EFI_TPL SavedTpl = mCurrentTpl;
  mCurrentTpl = Event->Tpl;
  Event->Notify(Event->Handle, Event->Context);
  mCurrentTpl = SavedTpl;
}

int main (VOID) {
  HostMemSet(&mBootServices, 0, sizeof(mBootServices));
  mBootServices.CreateEvent = MockCreateEvent;
  mBootServices.CreateEventEx = MockCreateEventEx;
  mBootServices.CloseEvent = MockCloseEvent;
  mBootServices.RegisterProtocolNotify = MockRegisterProtocolNotify;
  mBootServices.LocateProtocol = MockLocateProtocol;
  mBootServices.InstallMultipleProtocolInterfaces = MockInstallMultipleProtocolInterfaces;
  mBootServices.RaiseTPL = MockRaiseTpl;
  mBootServices.RestoreTPL = MockRestoreTpl;
  gBS = &mBootServices;

  HostMemSet(gConsole, 0, sizeof(gConsole));
  EFI_SYSTEM_TABLE SystemTable = {0};
  SystemTable.BootServices = gBS;
  assert(RamoopsCdcSerialPortLibConstructor(NULL, &SystemTable) == EFI_SUCCESS);
  assert(SerialPortWrite((UINT8 *)"early", 5) == 5);
  assert(((DALI_RAM_CONSOLE *)gConsole)->Size == 5);
  assert(mInstalledProtocol == NULL);

  assert(DaliUsbCdcLogInitialize((EFI_HANDLE)(UINTN)0x1111, NULL) == EFI_SUCCESS);
  assert(mInstalledProtocol != NULL);
  assert(mInstalledProtocol->Revision == DALI_USB_CDC_LOG_PROTOCOL_REVISION);
  assert(mInstalledProtocol->Size == sizeof(*mInstalledProtocol));

  assert(SerialPortWrite((UINT8 *)"no sink", 7) == 7);
  assert(mInstalledProtocol->DroppedBytes(mInstalledProtocol) == 7);

  struct usb_mtu3_device Device = {0};
  struct usb_cdc Cdc = {0};
  usb_log_init(&Cdc.log);
  Device.initialized = true;
  Device.cdc = &Cdc;
  assert(mInstalledProtocol->Bind(mInstalledProtocol, &Device) == EFI_SUCCESS);

  UINTN CallsBeforeHighTplWrite = mBootServiceCalls;
  mCurrentTpl = TPL_HIGH_LEVEL;
  assert(SerialPortWrite((UINT8 *)"bound", 5) == 5);
  assert(mCurrentTpl == TPL_HIGH_LEVEL);
  assert(mBootServiceCalls == CallsBeforeHighTplWrite);
  mCurrentTpl = TPL_APPLICATION;

  UINT8 Taken[16] = {0};
  assert(usb_log_take(&Cdc.log, Taken, sizeof(Taken)) == 5);
  assert(HostMemCmp(Taken, "bound", 5) == 0);
  assert(mInstalledProtocol->DroppedBytes(mInstalledProtocol) == 7);

  assert(mInstalledProtocol->Unbind(mInstalledProtocol, &Device) == EFI_SUCCESS);
  assert(SerialPortWrite((UINT8 *)"stopped", 7) == 7);
  assert(mInstalledProtocol->DroppedBytes(mInstalledProtocol) == 14);
  assert(mInstalledProtocol->Bind(mInstalledProtocol, &Device) == EFI_SUCCESS);
  assert(mInstalledProtocol->Unbind(mInstalledProtocol, &Device) == EFI_SUCCESS);

  assert(mEventCount == 3);
  SignalEvent(mEvents[0].Handle); /* SerialPortLib closes its cached protocol pointer. */
  SignalEvent(mEvents[2].Handle); /* Service rejects new writes at EBS. */
  assert(SerialPortWrite((UINT8 *)"after ebs", 9) == 9);
  assert(mInstalledProtocol->Write(mInstalledProtocol, "closed", 6) == 0);
  assert(((DALI_RAM_CONSOLE *)gConsole)->Size == 33);
  assert(mCurrentTpl == TPL_APPLICATION);
  return 0;
}
