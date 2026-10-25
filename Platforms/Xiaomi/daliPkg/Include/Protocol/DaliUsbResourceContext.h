// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/DaliUsbPlatformLib.h>
#include <DaliUsbI2cPins.h>
#include <Library/DaliUsbI2cLib.h>

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
typedef struct { BOOLEAN Active; } DALI_USB_I2C_TOKEN;
typedef EFI_STATUS (EFIAPI *DALI_USB_RESOURCE_GUARD)(VOID *Context);
typedef DALI_USB_RESOURCE_GUARD DALI_USB_VUSB_GUARD;
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
  /* Revision3: copied I2C6/NXP/GPIO/IOCFG resource graph and read-only pin
   * inspection, no I2C transfer or mux/power operation. Only first4KiB of each
   * returned FDT range is CPU-mapped. Read requires BSP/APPLICATION and a guard
   * proving powered GPIO/IOCFG and pin-writer exclusion for the whole call.
   * Guard/Context/Out are distinct persistent-for-call objects; guard is called
   * before and after the two masked captures. Mode1/EH bit0 are checked; other
   * inherited fields are reported, not electrically qualified. No lease persists.
   * EBS/guard/error/change leaves Out untouched; no entry-time register reads. */
  EFI_STATUS (EFIAPI *GetI2cResources)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_RESOURCES *);
  EFI_STATUS (EFIAPI *ReadI2cPins)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *,
    DALI_USB_RESOURCE_GUARD Guard, VOID *Context, struct dali_i2c6_pin_snapshot *Out);
  /* Revision4: actual native FIFO I2C6/GPIO72/gate binding for the production
   * NXP/PHY code. No peripheral probe/reset at entry or bus acquisition (GPIO
   * init is read-only). Policy holds all mappings/source/reference/electrical/
   * physical shared-gate/bus/pin conditions for the entire lease. Quiesce proves
   * every affected consumer stopped; Release additionally proves all dependent NXP/PHY
   * objects destroyed. These are caller proofs, not a policy implementation.
   * Persistent zeroed noncopyable Token owns one bus; its callbacks serialize
   * with VUSB operations and ReadI2cPins. Copied metadata getters take no lease.
   * BSP calls cannot overlap ExitBootServices. Context/Policy persist until
   * successful release. Failed acquisition can leave Token active; Release it.
   * Bus/Info/Token/Context storage is distinct. All copied bus interfaces expire
   * on release; destroy dependent users before token/storage reuse. No stale
   * interface may be used in a later lease, including a reused token address.
   * Faults stop new transfers, retain gates/controller/pin and uncertain writes.
   * Only recovery/reset-low/status/release remain usable. Complete release
   * before final memory map/EBS; after EBS every callback refuses access. */
  EFI_STATUS (EFIAPI *AcquireI2cBus)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_TOKEN *,
    DALI_USB_I2C_POLICY, VOID *, UINT32 UlposcHz, struct dali_nxp_repeater_bus *, DALI_USB_I2C_INFO *);
  EFI_STATUS (EFIAPI *GetI2cBusStatus)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_TOKEN *, DALI_USB_I2C_INFO *);
  EFI_STATUS (EFIAPI *ReleaseI2cBus)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_I2C_TOKEN *);
  /* Revision5: copied, FDT-validated xHCI register window. The resource context
   * maps its CPU page but does not authorize register access or provide an
   * xHCI/IRQ lease, clock/power/role control, or DMA mapping/qualification. */
  EFI_STATUS (EFIAPI *GetXhciResources)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_XHCI_RESOURCES *);
};
