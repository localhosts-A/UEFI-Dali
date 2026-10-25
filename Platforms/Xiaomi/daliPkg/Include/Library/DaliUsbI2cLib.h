// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/DaliUsbPlatformLib.h>
#include <DaliUsbClockRates.h>
#include <DaliUsbI2c.h>
#include <DaliUsbI2cPins.h>
#include <DaliUsbI2cClocks.h>
typedef enum {
  DaliUsbI2cValidate, // Entire lease: powered mappings, stable held sources,
                    // suitable electrical policy, exclusive bus/pins/gates.
  DaliUsbI2cQuiesce, // Additionally prove USB/PHY/I2C/shared APDMA consumers stopped.
  DaliUsbI2cRelease  // Additionally prove every dependent NXP/PHY/bus user destroyed.
} DALI_USB_I2C_POLICY_PHASE;
typedef EFI_STATUS (EFIAPI *DALI_USB_I2C_POLICY)(VOID *, DALI_USB_I2C_POLICY_PHASE);
typedef struct _DALI_USB_NATIVE_I2C DALI_USB_NATIVE_I2C;
typedef struct {
  DALI_USB_NATIVE_I2C *Owner;
  DALI_USB_MMIO_RANGE Range;
} DALI_USB_NATIVE_I2C_BANK;
typedef struct {
  UINT32 MainHz, DmaHz;
  UINT8 MainParent, DmaParent, OwnedGates;
  BOOLEAN Ready, Faulted, WriteUncertain, ResetLevelKnown, ResetEnabled;
  struct dali_i2c6_pin_snapshot Pins;
} DALI_USB_I2C_INFO;
struct _DALI_USB_NATIVE_I2C {
  // Permanent EBS latch: the owning DXE event sets this, never clears it.
  BOOLEAN Closed, Initialized, Ready, Faulted, WriteUncertain, Cleaning;
  DALI_USB_I2C_POLICY Policy;
  VOID *PolicyContext;
  UINT32 SwVersion, ReferenceHz, UlposcHz;
  DALI_USB_NATIVE_I2C_BANK Bank[9];
  struct dali_usb_clock_maps Maps;
  struct dali_usb_clock_rates Rates;
  struct dali_i2c6_pin_snapshot Pins;
  struct dali_i2c_clocks Clocks;
  struct dali_usb_i2c6 I2c;
  struct dali_usb_gpio72 Gpio;
  struct dali_nxp_io Link;
  struct dali_nxp_repeater_bus Bus;
};
/* Serialized BSP/APPLICATION caller, zeroed persistent noncopyable context.
 * Descriptors come from the validated private-FDT provider. Policy must hold
 * powered device mappings, stable selected sources/reference/optional ULPOSC,
 * correct pin electrical settings, entire I2C6 and GPIO72/104/105 ownership and
 * shared physical IMPW bit2/PERAO bit26 ownership (including UART/APDMA users).
 * Quiesce must remain valid after partial gate-enable failure: the controller
 * cannot be inspected safely unless both clocks are known enabled.
 * These are external obligations, not inferred from FDT/idle/double reads.
 * Policy is a bounded nonblocking proof callback (at most500us per call), not
 * the place to initiate resource transitions or reenter this bus. The worker
 * completes quiescence and holds first, then the callback verifies them.
 * Native library owns real MMIO/delay, gate and GPIO/I2C backends; no DMA data
 * mapping, TOP/PLL/mux/HWV write or peripheral transaction replay. Open retains
 * partial ownership on failure. Close is explicit, resets GPIO72 low and
 * restores only gates obtained here after all dependent users are destroyed.
 * Caller registers EBS before publication and sets Closed independently of
 * other events. No callback/context/free/reuse after EBS, including retries.
 * Access the controller only through these checked entries, never Bus/fields
 * directly. A public DXE bus wrapper additionally serializes identity tokens.
 * NXP/PHY objects must be destroyed before Close; old bus copies are invalid.
 * All failures retain context until successful Close. Faulted instances allow
 * only Recover, reset-low, status and Close; none authorizes a new transfer. */
int DaliUsbNativeI2cOpen(DALI_USB_NATIVE_I2C *, CONST DALI_USB_CLOCK_RESOURCES *,
  CONST DALI_USB_I2C_RESOURCES *, DALI_USB_I2C_POLICY, VOID *, UINT32 UlposcHz);
int DaliUsbNativeI2cCheck(DALI_USB_NATIVE_I2C *);
int DaliUsbNativeI2cRead(DALI_USB_NATIVE_I2C *, UINT8, UINT8 *);
int DaliUsbNativeI2cWrite(DALI_USB_NATIVE_I2C *, UINT8, UINT8);
int DaliUsbNativeI2cReset(DALI_USB_NATIVE_I2C *, BOOLEAN Enable);
int DaliUsbNativeI2cRecover(DALI_USB_NATIVE_I2C *);
int DaliUsbNativeI2cClose(DALI_USB_NATIVE_I2C *);
int DaliUsbNativeI2cInfo(DALI_USB_NATIVE_I2C *, DALI_USB_I2C_INFO *);
VOID DaliUsbNativeI2cDelay(DALI_USB_NATIVE_I2C *, UINT32);
