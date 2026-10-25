// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DALI_USB_PMIC_LIB_H
#define DALI_USB_PMIC_LIB_H
#include <Uefi.h>
#include <Protocol/DaliPmicBus.h>
#include <DaliUsbPmif.h>
#include <Library/DaliUsbPlatformLib.h>

typedef struct {
  struct dali_usb_pmif Transport;
  DALI_PMIC_BUS_PROTOCOL *Bus;
  DALI_PMIC_BUS_TOKEN Token;
  BOOLEAN Ready;
  BOOLEAN Faulted;
} DALI_USB_PMIC;

// Persistent zeroed BSP-owned context. Open at <=TPL_NOTIFY, before USB/TCPC
// startup. Validates the real FDT PMIF/SID14 route, binds the singleton lease,
// reads controller readiness and actual MT6379 DEV_INFO. No PMIC register write
// or clock/route change. An error may retain a pending owned command/context;
// close it before retrying. A release-invariant fault retains it permanently.
EFI_STATUS DaliUsbPmicOpen (DALI_USB_PMIC *Context, CONST VOID *Fdt);
// Same persistent/shared-channel contract, using only a descriptor acquired by
// DaliUsbReadVusbResources from the native immutable snapshot. CID high73 at9,
// CID low8 and nonzero SWCIDb are read before allowing the sole enable operation.
// It does not enable the rail or claim a voltage/source/power-good qualification.
EFI_STATUS DaliUsbVusbPmicOpen (DALI_USB_PMIC *Context, CONST DALI_USB_VUSB_RESOURCES *Resources);
// First destroy all dependent TCPC/VBUS/USB objects after their real shutdown.
// This only recovers/releases transport ownership; it does NOT turn power off
// or stop USB DMA. Failed close retains the context/token for explicit recovery.
EFI_STATUS DaliUsbPmicClose (DALI_USB_PMIC *Context);
#endif
