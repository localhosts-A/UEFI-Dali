// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/DaliUsbCdcLogLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DaliUsbCdcLog.h>

#define DALI_CDC_LOG_CLOSED 0x80000000U
#define DALI_CDC_LOG_WRITER_MASK 0x7fffffffU

STATIC volatile UINT32 mState;
STATIC EFI_EVENT mExitEvent;
STATIC EFI_HANDLE mHandle;

STATIC EFI_STATUS EFIAPI Bind (DALI_USB_CDC_LOG_PROTOCOL *This, VOID *Device);
STATIC EFI_STATUS EFIAPI Unbind (DALI_USB_CDC_LOG_PROTOCOL *This, VOID *Device);
STATIC UINTN EFIAPI Write (DALI_USB_CDC_LOG_PROTOCOL *This, CONST VOID *Bytes, UINTN Length);
STATIC UINT64 EFIAPI DroppedBytes (DALI_USB_CDC_LOG_PROTOCOL *This);

STATIC DALI_USB_CDC_LOG_PROTOCOL mProtocol = {
  DALI_USB_CDC_LOG_PROTOCOL_REVISION,
  sizeof (DALI_USB_CDC_LOG_PROTOCOL),
  Bind,
  Unbind,
  Write,
  DroppedBytes
};

STATIC EFI_STATUS MapUsbStatus (int Status) {
  switch (Status) {
    case USB_OK: return EFI_SUCCESS;
    case USB_BUSY: return EFI_NOT_READY;
    case USB_INVALID: return EFI_INVALID_PARAMETER;
    case USB_UNSUPPORTED: return EFI_UNSUPPORTED;
    default: return EFI_DEVICE_ERROR;
  }
}

STATIC EFI_TPL CurrentTpl (VOID) {
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  return Tpl;
}

STATIC EFI_STATUS EFIAPI Bind (DALI_USB_CDC_LOG_PROTOCOL *This, VOID *Device) {
  if (This != &mProtocol || Device == NULL) { return EFI_INVALID_PARAMETER; }
  if (CurrentTpl () != TPL_APPLICATION) { return EFI_UNSUPPORTED; }

  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (__atomic_load_n (&mState, __ATOMIC_ACQUIRE) & DALI_CDC_LOG_CLOSED) {
    gBS->RestoreTPL (OldTpl);
    return EFI_UNSUPPORTED;
  }
  int Status = dali_usb_cdc_log_bind ((struct usb_mtu3_device *)Device);
  gBS->RestoreTPL (OldTpl);
  return MapUsbStatus (Status);
}

STATIC EFI_STATUS EFIAPI Unbind (DALI_USB_CDC_LOG_PROTOCOL *This, VOID *Device) {
  if (This != &mProtocol || Device == NULL) { return EFI_INVALID_PARAMETER; }
  if (CurrentTpl () != TPL_APPLICATION) { return EFI_UNSUPPORTED; }

  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  int Status = dali_usb_cdc_log_unbind ((struct usb_mtu3_device *)Device);
  gBS->RestoreTPL (OldTpl);
  return MapUsbStatus (Status);
}

STATIC BOOLEAN EnterWriter (VOID) {
  UINT32 State = __atomic_load_n (&mState, __ATOMIC_ACQUIRE);
  for (;;) {
    if ((State & DALI_CDC_LOG_CLOSED) ||
        (State & DALI_CDC_LOG_WRITER_MASK) == DALI_CDC_LOG_WRITER_MASK) {
      return FALSE;
    }
    if (__atomic_compare_exchange_n (&mState, &State, State + 1, FALSE,
          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return TRUE;
    }
  }
}

STATIC UINTN EFIAPI Write (DALI_USB_CDC_LOG_PROTOCOL *This, CONST VOID *Bytes, UINTN Length) {
  if (This != &mProtocol || Length == 0 || Bytes == NULL || !EnterWriter ()) { return 0; }
  UINTN Accepted = dali_usb_cdc_log_write (Bytes, Length);
  __atomic_fetch_sub (&mState, 1, __ATOMIC_RELEASE);
  return Accepted;
}

STATIC UINT64 EFIAPI DroppedBytes (DALI_USB_CDC_LOG_PROTOCOL *This) {
  return This == &mProtocol ? dali_usb_cdc_log_dropped () : 0;
}

STATIC VOID EFIAPI ExitBootServices (EFI_EVENT Event, VOID *Context) {
  (VOID)Event;
  (VOID)Context;
  __atomic_fetch_or (&mState, DALI_CDC_LOG_CLOSED, __ATOMIC_RELEASE);
}

EFI_STATUS EFIAPI DaliUsbCdcLogInitialize (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  DALI_USB_CDC_LOG_PROTOCOL *Existing;
  (VOID)SystemTable;
  if (ImageHandle == NULL) { return EFI_INVALID_PARAMETER; }

  EFI_STATUS Status = gBS->LocateProtocol (&gDaliUsbCdcLogProtocolGuid, NULL, (VOID **)&Existing);
  if (!EFI_ERROR (Status)) { return EFI_ALREADY_STARTED; }
  if (Status != EFI_NOT_FOUND) { return Status; }

  __atomic_store_n (&mState, 0, __ATOMIC_RELEASE);
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, NULL,
    &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) { return Status; }

  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle,
    &gDaliUsbCdcLogProtocolGuid, &mProtocol, NULL);
  if (EFI_ERROR (Status)) {
    __atomic_fetch_or (&mState, DALI_CDC_LOG_CLOSED, __ATOMIC_RELEASE);
    gBS->CloseEvent (mExitEvent);
    mExitEvent = NULL;
  }
  return Status;
}
