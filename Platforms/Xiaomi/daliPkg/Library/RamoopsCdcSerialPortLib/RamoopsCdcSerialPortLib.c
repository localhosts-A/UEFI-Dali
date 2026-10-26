// SPDX-License-Identifier: BSD-2-Clause-Patent
// DXE-only best-effort tee. Ramoops remains the authoritative early and fallback sink.
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/SerialPortLib.h>
#include <Library/DaliConsole.h>
#include <Protocol/DaliUsbCdcLog.h>

STATIC DALI_USB_CDC_LOG_PROTOCOL * volatile mProtocol;
STATIC EFI_BOOT_SERVICES *mBootServices;
STATIC EFI_EVENT mNotifyEvent;
STATIC EFI_EVENT mExitEvent;
STATIC VOID *mRegistration;
STATIC volatile BOOLEAN mReady;
STATIC volatile BOOLEAN mClosing;

STATIC VOID RefreshProtocol (VOID) {
  DALI_USB_CDC_LOG_PROTOCOL *Protocol = NULL;
  if (!__atomic_load_n (&mReady, __ATOMIC_ACQUIRE) ||
      __atomic_load_n (&mClosing, __ATOMIC_ACQUIRE)) {
    return;
  }
  if (mBootServices == NULL) { return; }
  if (!EFI_ERROR (mBootServices->LocateProtocol (&gDaliUsbCdcLogProtocolGuid, NULL, (VOID **)&Protocol)) &&
      Protocol != NULL && Protocol->Revision >= DALI_USB_CDC_LOG_PROTOCOL_REVISION &&
      Protocol->Size >= sizeof (*Protocol) && Protocol->Write != NULL) {
    __atomic_store_n (&mProtocol, Protocol, __ATOMIC_RELEASE);
  }
}

STATIC VOID EFIAPI ProtocolNotify (EFI_EVENT Event, VOID *Context) {
  (VOID)Event;
  (VOID)Context;
  RefreshProtocol ();
}

STATIC VOID EFIAPI ExitBootServices (EFI_EVENT Event, VOID *Context) {
  (VOID)Event;
  (VOID)Context;
  __atomic_store_n (&mClosing, TRUE, __ATOMIC_RELEASE);
  __atomic_store_n (&mProtocol, NULL, __ATOMIC_RELEASE);
}

EFI_STATUS EFIAPI RamoopsCdcSerialPortLibConstructor (
  EFI_HANDLE ImageHandle,
  EFI_SYSTEM_TABLE *SystemTable
  )
{
  (VOID)ImageHandle;
  if (SystemTable == NULL || SystemTable->BootServices == NULL) { return EFI_SUCCESS; }
  mBootServices = SystemTable->BootServices;
  __atomic_store_n (&mProtocol, NULL, __ATOMIC_RELAXED);
  __atomic_store_n (&mClosing, FALSE, __ATOMIC_RELAXED);
  __atomic_store_n (&mReady, FALSE, __ATOMIC_RELAXED);

  EFI_STATUS Status = mBootServices->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices,
    NULL, &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) { return EFI_SUCCESS; }

  Status = mBootServices->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ProtocolNotify, NULL, &mNotifyEvent);
  if (!EFI_ERROR (Status)) {
    Status = mBootServices->RegisterProtocolNotify (&gDaliUsbCdcLogProtocolGuid, mNotifyEvent, &mRegistration);
    if (EFI_ERROR (Status)) {
      mBootServices->CloseEvent (mNotifyEvent);
      mNotifyEvent = NULL;
    }
  }

  // If notification registration failed, an already-installed service can still be used.
  // Without EBS registration this library remains ramoops-only for safety.
  __atomic_store_n (&mReady, TRUE, __ATOMIC_RELEASE);
  RefreshProtocol ();
  return EFI_SUCCESS;
}

UINTN EFIAPI SerialPortWrite (IN UINT8 *Buffer, IN UINTN NumberOfBytes) {
  if (Buffer == NULL || NumberOfBytes == 0) { return 0; }

  DALI_RAM_CONSOLE *Ring = (DALI_RAM_CONSOLE *)(UINTN)DALI_CONSOLE_BASE;
  UINT32 Capacity = DALI_CONSOLE_SIZE - 12;
  BOOLEAN Interrupts = ArmGetInterruptState ();
  ArmDisableInterrupts ();
  DaliConsoleAppend (Ring, Capacity, Buffer, NumberOfBytes);
  if (Interrupts) { ArmEnableInterrupts (); }

  DALI_USB_CDC_LOG_PROTOCOL *Protocol = __atomic_load_n (&mProtocol, __ATOMIC_ACQUIRE);
  if (Protocol != NULL && !__atomic_load_n (&mClosing, __ATOMIC_ACQUIRE) && Protocol->Write != NULL) {
    (VOID)Protocol->Write (Protocol, Buffer, NumberOfBytes);
  }
  return NumberOfBytes;
}

RETURN_STATUS EFIAPI SerialPortInitialize (VOID) { return RETURN_SUCCESS; }
UINTN EFIAPI SerialPortRead (OUT UINT8 *Buffer, IN UINTN NumberOfBytes) { (VOID)Buffer; (VOID)NumberOfBytes; return 0; }
BOOLEAN EFIAPI SerialPortPoll (VOID) { return FALSE; }
RETURN_STATUS EFIAPI SerialPortSetControl (IN UINT32 Control) { (VOID)Control; return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortGetControl (OUT UINT32 *Control) { (VOID)Control; return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortSetAttributes (IN OUT UINT64 *BaudRate, IN OUT UINT32 *ReceiveFifoDepth,
  IN OUT UINT32 *Timeout, IN OUT EFI_PARITY_TYPE *Parity, IN OUT UINT8 *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits) {
  (VOID)BaudRate; (VOID)ReceiveFifoDepth; (VOID)Timeout; (VOID)Parity; (VOID)DataBits; (VOID)StopBits;
  return RETURN_UNSUPPORTED;
}
