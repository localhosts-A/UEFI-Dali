// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/DaliUsbPlatformLib.h>

extern EFI_GUID gDaliUsbResourceContextProtocolGuid;
typedef struct _DALI_USB_RESOURCE_CONTEXT_PROTOCOL DALI_USB_RESOURCE_CONTEXT_PROTOCOL;
typedef struct {
  EFI_STATUS InitializationStatus;
  UINT32 MappingAttempts;
  UINT32 MappedPages;
  BOOLEAN SnapshotValid;
  BOOLEAN BootServicesClosed;
} DALI_USB_RESOURCE_CONTEXT_STATUS;
typedef struct { BOOLEAN Active; } DALI_USB_VUSB_TOKEN;
typedef EFI_STATUS (EFIAPI *DALI_USB_VUSB_GUARD)(VOID *Context);
typedef struct {
  UINT32 NominalMicrovolt;
  UINT8 Selector, Trim, LowPower, ChipRevision;
} DALI_USB_VUSB_INFO;
/* BSP Boot Services interface. Status/resource accessors describe CPU mappings and an
 * immutable private FDT-derived description only: not a powered-source/rate
 * lease, hardware qualification, DMA map, HC registration or permission to
 * access unpowered MMIO. Output is copied; the private FDT is never exposed.
 * Mapping attempts (including partial failures) are retained for firmware
 * lifetime; no unload or removal of live CPU page-table mappings is supported.
 * GCD ownership and CPU attributes cover whole 4KiB pages, including the tail
 * half-page beyond APMIXED's logical window. Register access remains bounded
 * by each returned logical window; page rounding grants no extra MMIO access.
 * The controller worker must separately acquire all resource/clock/DMA/role
 * prerequisites. Every accessor refuses after the first EBS notification. */
struct _DALI_USB_RESOURCE_CONTEXT_PROTOCOL {
  UINT32 Revision;
  EFI_STATUS (EFIAPI *GetStatus)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_RESOURCE_CONTEXT_STATUS *);
  EFI_STATUS (EFIAPI *GetClockResources)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_CLOCK_RESOURCES *);
  EFI_STATUS (EFIAPI *GetVusbResources)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_VUSB_RESOURCES *);
  /* Revision2: explicit BSP/APPLICATION power operation, never at entry. Token
   * is persistent, zeroed, noncopyable, distinct from Out, and identified by its
   * address. Guard/Context remain valid until successful Release. Guard must
   * stop USB/PHY consumers and prove the native supply policy excludes competing
   * VUSB enable/voltage/trim/LP writers for the entire lease (including firmware
   * agents); this is a caller obligation, not inferred from FDT or double reads.
   * Acquire serializes owners, validates actual CID and uses shared PMIF leases,
   * enables only bit0, waits720us and returns configured nominal voltage/trim.
   * It does not establish electrical power-good/voltage suitability or clock,
   * domain, connector, DMA or controller readiness. Failures may leave Token
   * active; explicitly Release before reuse. A live same-token acquire rechecks
   * the rail and cannot replace Guard. Release quiesces and recovers owned PMIF
   * work; the always-on rail is NEVER disabled. Complete Release before final
   * memory map/EBS. EBS closes access, retaining any unreleased state. */
  EFI_STATUS (EFIAPI *AcquireVusb)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_VUSB_TOKEN *,
    DALI_USB_VUSB_GUARD Guard, VOID *Context, DALI_USB_VUSB_INFO *Out);
  EFI_STATUS (EFIAPI *ReleaseVusb)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_VUSB_TOKEN *);
};
