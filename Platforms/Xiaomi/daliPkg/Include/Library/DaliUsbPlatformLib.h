// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <DaliUsbCalibration.h>
/* Read-only acquisition from a complete, immutable, 8-byte-aligned boot FDT
 * snapshot whose accessible allocation is FdtSize (at most16MiB). Out must be
 * distinct writable storage. Validate the Dali USB0/PHY/nvmem graph and decode
 * actual chosen ATAG payloads; no hardware accesses, allocation or retained
 * pointers. Check return status: output is committed only on complete success.
 * qualified describes calibration input only, not clock/DMA/power/hardware or
 * authenticity of the bootloader. The installed controller caller is separate. */
/* Requires the pinned BaseFdtLib implementation and its real fdt_check_full. */
EFI_STATUS DaliUsbReadCalibration (
  IN CONST VOID *Fdt, IN UINTN FdtSize,
  OUT struct dali_xsphy_calibration *Out
  );

enum DALI_USB_CLOCK_BANK {
  DaliUsbClockVlp, DaliUsbClockTop, DaliUsbClockApmixed,
  DaliUsbClockImpw, DaliUsbClockPerao, DaliUsbClockBankCount
};
typedef struct { UINT32 Base, Size; } DALI_USB_MMIO_RANGE;
typedef struct {
  DALI_USB_MMIO_RANGE Bank[DaliUsbClockBankCount];
  UINT32 ReferenceHz;
  struct dali_xsphy_calibration Calibration;
} DALI_USB_CLOCK_RESOURCES;
/* Same immutable, aligned bounded FDT contract as ReadCalibration. Return a
 * pointer-free physical resource description for the actual USB0 VLP/TOP,
 * PHY/xHCI fixed26MHz and I2C6 main/APDMA clock paths, after exact enabled
 * provider/window/ID/specifier/name/consumer graph validation and calibration
 * acquisition. No MMIO access, mapping, source vote, allocation or retained FDT
 * pointers. Output changes only on complete success, and must be distinct from
 * the snapshot. Caller still needs device-memory mappings, held powered sources,
 * source-stability/shared-provider leases and real runtime rate/pin/DMA checks.
 * A validated fixed-clock declaration is not physical frequency qualification.
 * This interface does not qualify supplies, Type-C/ADC, power domains or DMA. */
EFI_STATUS DaliUsbReadClockResources (
  IN CONST VOID *Fdt, IN UINTN FdtSize,
  OUT DALI_USB_CLOCK_RESOURCES *Out
  );

#define DALI_USB_XHCI_INTERRUPT_CELLS 4U
typedef struct {
  DALI_USB_MMIO_RANGE Registers;
  UINT32 InterruptSpecifier[DALI_USB_XHCI_INTERRUPT_CELLS];
} DALI_USB_XHCI_RESOURCES;
/* Validate the complete USB0/PHY/clock/calibration graph and copy the physical
 * xHCI register window plus the four CPU-endian cells from the FDT interrupts
 * property. The IRQ cells are an untranslated description, not a GIC vector,
 * interrupt handle or proof of ownership; the provider must resolve and acquire
 * the actual source separately. The accessor grants no permission to touch an
 * unpowered controller and qualifies neither IRQ, DMA, power, role nor hardware. */
EFI_STATUS DaliUsbReadXhciResources (
  IN CONST VOID *Fdt, IN UINTN FdtSize,
  OUT DALI_USB_XHCI_RESOURCES *Out
  );

typedef struct {
  UINT16 SpmiDeviceMask;
  UINT32 MinMicrovolt, MaxMicrovolt, EnableRampMicroseconds;
} DALI_USB_VUSB_RESOURCES;
/* Full immutable/bounded/calibration FDT contract above. Validate USB0's unique
 * vusb33 reference to enabled MT6373 SID5/regulators/vusb, native PMIF-M routing,
 * exact declared CID tuple73,9,8 and always-on/range/ramp policy. Output contains
 * copied descriptions only: no rail read/enable or power-good qualification. */
EFI_STATUS DaliUsbReadVusbResources (
  IN CONST VOID *Fdt, IN UINTN FdtSize, OUT DALI_USB_VUSB_RESOURCES *Out
  );

typedef struct {
  DALI_USB_MMIO_RANGE Gpio, IocfgLb2, Controller, Dma;
  UINT32 SclPin, SdaPin, ResetPin;
} DALI_USB_I2C_RESOURCES;
/* Full clock/calibration FDT validation plus exact MT6991 pinctrl windows/names,
 * GPIO104/105 I2C6 channel0/open-drain/stretch configuration and enabled NXP4f
 * GPIO72 enable/disable pinctrl graph; reject additional unimplemented pinconf
 * actions. Copied ranges describe full FDT windows;
 * native context maps only their first4KiB (all registers used by this backend).
 * This does not acquire a bus/pin/source/power lease or qualify electrical
 * policy. Other clients on the I2C6 bus must be excluded before any transfer. */
EFI_STATUS DaliUsbReadI2cResources (
  IN CONST VOID *Fdt, IN UINTN FdtSize, OUT DALI_USB_I2C_RESOURCES *Out
  );
