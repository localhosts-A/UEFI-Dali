// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <Base.h>
#include <UsbMtu3Device.h>

int dali_usb_cdc_log_bind (struct usb_mtu3_device *Device);
int dali_usb_cdc_log_unbind (struct usb_mtu3_device *Device);
UINTN dali_usb_cdc_log_write (CONST VOID *Bytes, UINTN Length);
UINT64 dali_usb_cdc_log_dropped (VOID);
