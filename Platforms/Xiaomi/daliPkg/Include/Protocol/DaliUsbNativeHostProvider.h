// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/DaliUsbXhciIoLib.h>
#include <Library/DaliUsbPlatformLib.h>

extern EFI_GUID gDaliUsbNativeHostProviderProtocolGuid;
typedef struct _DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL;

#define DALI_USB_NATIVE_HOST_PROVIDER_REVISION 1U
#define DALI_USB_NATIVE_HOST_LEASE_REVISION 1U
#define DALI_USB_NATIVE_HOST_MAX_PORTS 16U

#define DALI_USB_HOST_QUAL_MMIO             (1ULL << 0)
#define DALI_USB_HOST_QUAL_CLOCKS           (1ULL << 1)
#define DALI_USB_HOST_QUAL_POWER_DOMAIN     (1ULL << 2)
#define DALI_USB_HOST_QUAL_PHY              (1ULL << 3)
#define DALI_USB_HOST_QUAL_HOST_ROLE        (1ULL << 4)
#define DALI_USB_HOST_QUAL_CONNECTOR        (1ULL << 5)
#define DALI_USB_HOST_QUAL_VBUS             (1ULL << 6)
#define DALI_USB_HOST_QUAL_CONTROLLER       (1ULL << 7)
#define DALI_USB_HOST_QUAL_IRQ              (1ULL << 8)
#define DALI_USB_HOST_QUAL_DMA32            (1ULL << 9)
#define DALI_USB_HOST_QUAL_CACHE            (1ULL << 10)
#define DALI_USB_HOST_QUAL_SOURCE_STABILITY (1ULL << 11)
#define DALI_USB_HOST_REQUIRED_QUALIFICATIONS ((1ULL << 12) - 1ULL)

/* Persistent address-identified provider lease. Acquire may return an error
 * while leaving Active set; the caller must Release it before reusing storage.
 * Release success must clear Active. This object and its callback contexts may
 * not move or be copied. */
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  UINT64 QualificationMask;
  UINT64 RegisterBase;
  UINT64 RegisterLength;
  DALI_USB_XHCI_DMA_OPS Dma;
  DALI_USB_XHCI_IO_GUARD Guard;
  VOID *GuardContext;
  UINT8 Usb2PortCount;
  UINT8 Usb2Ports[DALI_USB_NATIVE_HOST_MAX_PORTS];
  BOOLEAN Active;
} DALI_USB_NATIVE_HOST_LEASE;

/* Provider contract: Acquire succeeds only while the exact requested MMIO
 * window is powered/mapped and exclusively owned, the clocks/domain/PHY and
 * USB host role are already valid, connector/VBUS policy is satisfied, the
 * controller and IRQ are exclusive, and real <=32-bit device DMA translation
 * plus cache operations are held. It must never infer identity DMA from the
 * CPU address or force a Type-C role/VBUS change. An inactive/not-ready role
 * returns EFI_NOT_READY without touching the controller. Guard and DMA callbacks
 * are bounded/nonblocking through TPL_NOTIFY and preserve ownership on errors.
 * Release is allowed only after the consumer has stopped and closed xHCI. */
struct _DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL {
  UINT32 Revision;
  EFI_STATUS (EFIAPI *Acquire)(DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *,
    CONST DALI_USB_XHCI_RESOURCES *, DALI_USB_NATIVE_HOST_LEASE *);
  EFI_STATUS (EFIAPI *Release)(DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *,
    DALI_USB_NATIVE_HOST_LEASE *);
};
