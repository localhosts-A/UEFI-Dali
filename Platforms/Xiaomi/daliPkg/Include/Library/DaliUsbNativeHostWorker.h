// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Protocol/DaliUsbNativeHostProvider.h>
#include <Protocol/DaliUsbResourceContext.h>
#include <Library/DaliUsbNativeHostLib.h>

extern EFI_GUID gDaliUsbNativeHostWorkerProtocolGuid;
typedef struct _DALI_USB_NATIVE_HOST_WORKER_PROTOCOL DALI_USB_NATIVE_HOST_WORKER_PROTOCOL;

#define DALI_USB_NATIVE_HOST_WORKER_REVISION 1U
typedef enum {
  DaliUsbHostWorkerDormant = 0,
  DaliUsbHostWorkerStarting,
  DaliUsbHostWorkerWaitingForRole,
  DaliUsbHostWorkerStarted,
  DaliUsbHostWorkerStopped,
  DaliUsbHostWorkerFailed,
  DaliUsbHostWorkerRetained
} DALI_USB_NATIVE_HOST_WORKER_STATE;

struct _DALI_USB_NATIVE_HOST_WORKER_PROTOCOL {
  UINT32 Revision;
  DALI_USB_NATIVE_HOST_WORKER_STATE State;
  EFI_STATUS LastStatus;
  EFI_STATUS CleanupStatus;
  UINT64 QualificationMask;
  EFI_STATUS (EFIAPI *Start)(DALI_USB_NATIVE_HOST_WORKER_PROTOCOL *);
  EFI_STATUS (EFIAPI *Stop)(DALI_USB_NATIVE_HOST_WORKER_PROTOCOL *);
};

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL VendorNode;
  EFI_GUID Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} DALI_USB_NATIVE_HOST_DEVICE_PATH;

/* Persistent static/BSS owner installed on the worker image handle. Never copy
 * or unload this context. Start/Stop are BSP TPL_APPLICATION calls. */
typedef struct {
  DALI_USB_NATIVE_HOST_WORKER_PROTOCOL Protocol;
  DALI_USB_RESOURCE_CONTEXT_PROTOCOL *Resources;
  DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *Provider;
  DALI_USB_NATIVE_HOST_LEASE Lease;
  DALI_USB_XHCI_IO Io;
  DALI_USB_NATIVE_HOST Host;
  DALI_USB_NATIVE_HOST_DEVICE_PATH Path;
} DALI_USB_NATIVE_HOST_WORKER;

EFI_STATUS DaliUsbNativeHostWorkerStart(DALI_USB_NATIVE_HOST_WORKER *,
  DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *);
EFI_STATUS DaliUsbNativeHostWorkerStop(DALI_USB_NATIVE_HOST_WORKER *);
