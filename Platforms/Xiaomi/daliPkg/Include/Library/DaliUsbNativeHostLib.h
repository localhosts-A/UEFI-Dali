// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/DaliUsbXhciIoLib.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/DevicePath.h>

typedef struct {
  EFI_USB2_HC_PROTOCOL Protocol;
  DALI_USB_XHCI_IO *Io;
  VOID *Engine;
  EFI_HANDLE Handle;
  EFI_EVENT Timer, ExitEvent;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  UINT32 Busy, Epoch, CallbackDepth;
  UINT8 PortCount, Ports[16]; // Physical xHC port indices, zero based; USB2 only.
  BOOLEAN Closed, Faulted, Accepting, Published, Prepared;
} DALI_USB_NATIVE_HOST;

/* BSP only, persistent, zeroed, noncopyable context. Start/Stop/Prepare are
 * TPL_APPLICATION calls, protocol methods <=TPL_NOTIFY, callbacks <=TPL_CALLBACK.
 * IO must already be Open with actual held MMIO/DMA/platform qualifications;
 * no other consumer may use it until Stop succeeds. Keep IO/provider/path and
 * context alive on ANY failure. Stop may be retried; never clear Closed.
 * This host owns IO's EBS latch. A platform event must not close IO or release
 * its power/clock/DMA providers ahead of this host's halt; event order alone
 * does not prove quiesce. The platform handoff worker must enforce this.
 * Ports must identify USB2 standard-speed-ID ports in HC Supported Protocol
 * capabilities. This engine requires 4KiB HC pages and supplies no MTK vendor
 * endpoint scheduler: a Dali hardware worker must not start it until that
 * implementation is bound. No hardware qualification is asserted.
 * PrepareHandoff irreversibly rejects submissions and freshly proves halt,
 * without allocation/free. Call it before the final GetMemoryMap. EBS is only
 * a last shutdown latch; it cannot approve a failed OS handoff.
 * Stop disconnects class drivers, halts, uninstalls the public USB2/path pair,
 * destroys source users, and leaves IO available for its separate Close. */
EFI_STATUS DaliUsbNativeHostStart(DALI_USB_NATIVE_HOST *, DALI_USB_XHCI_IO *,
  EFI_DEVICE_PATH_PROTOCOL *, CONST UINT8 *Ports, UINT8 PortCount);
EFI_STATUS DaliUsbNativeHostPrepareHandoff(DALI_USB_NATIVE_HOST *);
EFI_STATUS DaliUsbNativeHostStop(DALI_USB_NATIVE_HOST *);
EFI_STATUS DaliUsbNativeHostPoll(DALI_USB_NATIVE_HOST *);
