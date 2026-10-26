// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <Uefi.h>

#define DALI_USB_CDC_LOG_PROTOCOL_REVISION 1U

typedef struct _DALI_USB_CDC_LOG_PROTOCOL DALI_USB_CDC_LOG_PROTOCOL;

/*
 * Provider lifetime operations require TPL_APPLICATION. Bind only an initialized
 * device whose provider still owns it. Unbind before stop/destroy/ExitBootServices;
 * EFI_NOT_READY means a writer or lifecycle transition is in flight, so keep the
 * device alive and retry.
 */
typedef EFI_STATUS (EFIAPI *DALI_USB_CDC_LOG_BIND)(
  IN DALI_USB_CDC_LOG_PROTOCOL *This,
  IN VOID *Device
  );

typedef EFI_STATUS (EFIAPI *DALI_USB_CDC_LOG_UNBIND)(
  IN DALI_USB_CDC_LOG_PROTOCOL *This,
  IN VOID *Device
  );

/* Nonblocking and Boot-Services-free; safe for SerialPortWrite at elevated TPL. */
typedef UINTN (EFIAPI *DALI_USB_CDC_LOG_WRITE)(
  IN DALI_USB_CDC_LOG_PROTOCOL *This,
  IN CONST VOID *Bytes,
  IN UINTN Length
  );

/* Count of bytes rejected by the adapter, excluding CDC ring-full drops. */
typedef UINT64 (EFIAPI *DALI_USB_CDC_LOG_DROPPED)(
  IN DALI_USB_CDC_LOG_PROTOCOL *This
  );

struct _DALI_USB_CDC_LOG_PROTOCOL {
  UINT32 Revision;
  UINT32 Size;
  DALI_USB_CDC_LOG_BIND Bind;
  DALI_USB_CDC_LOG_UNBIND Unbind;
  DALI_USB_CDC_LOG_WRITE Write;
  DALI_USB_CDC_LOG_DROPPED DroppedBytes;
};

extern EFI_GUID gDaliUsbCdcLogProtocolGuid;
