// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/Mt6991PlatformLib.h>

// Official MT6991 pin ranges: GPIO60 DIN[28] at 0x210, DIR[28] at
// 0x10, MODE[19:16] at 0x370. Never change pinmux, pulls or direction.
STATIC BOOLEAN mSample, mStable, mLong, mReady;
STATIC UINT64 mDebounce, mHeld;
#define MS(Value) ((UINT64)(Value) * 1000000ULL)

RETURN_STATUS EFIAPI KeypadDeviceConstructor (VOID) {
  CONST VOID *Fdt = (VOID *)(UINTN)DALI_FDT_BASE;
  CONST UINT32 *Pins;
  INT32 Node, Controller, Length;
  UINT32 Mode, Direction;
  if (FdtCheckHeader (Fdt)) { return RETURN_SUCCESS; }
  Node = FdtPathOffset (Fdt, "/gpio-keys/volumedown");
  Controller = FdtPathOffset (Fdt, "/soc/pinctrl@1002d000");
  Pins = FdtGetProp (Fdt, Node, "gpios", &Length);
  if (Controller < 0 || Pins == NULL || Length != 12 ||
      Fdt32ToCpu (Pins[0]) != FdtGetPhandle (Fdt, Controller) ||
      Fdt32ToCpu (Pins[1]) != 60 || Fdt32ToCpu (Pins[2]) != 1) {
    DEBUG ((DEBUG_ERROR, "DALI: keypad FDT mismatch; input disabled\n"));
    return RETURN_SUCCESS;
  }
  DEBUG ((DEBUG_WARN, "DALI: probing GPIO60 input\n"));
  Mode = (MmioRead32 (DALI_GPIO_BASE + 0x370) >> 16) & 15;
  Direction = (MmioRead32 (DALI_GPIO_BASE + 0x10) >> 28) & 1;
  DEBUG ((DEBUG_WARN, "DALI: GPIO60 mode=%u direction=%u din=%u\n", Mode, Direction,
    (MmioRead32 (DALI_GPIO_BASE + 0x210) >> 28) & 1));
  // LK leaves mode1=KPCOL0 (official MT6991 pin function), Linux uses mode0.
  // Sample the input pad in either input mode without changing the mux.
  mReady = (Mode == 0 || Mode == 1) && Direction == 0;
  DEBUG ((DEBUG_WARN, "DALI: GPIO60 input enabled=%u\n", mReady));
  // This optional input must not assert in the generated constructor list.
  return RETURN_SUCCESS;
}

EFI_STATUS EFIAPI KeypadDeviceReset (KEYPAD_DEVICE_PROTOCOL *This) {
  mSample = mStable = mLong = FALSE;
  mDebounce = mHeld = 0;
  return EFI_SUCCESS;
}

EFI_STATUS KeypadDeviceGetKeys (KEYPAD_DEVICE_PROTOCOL *This, KEYPAD_RETURN_API *Api, UINT64 Delta) {
  BOOLEAN Pressed;
  EFI_KEY_DATA Key = {0};
  if (!mReady) { return EFI_NOT_READY; }
  Pressed = (MmioRead32 (DALI_GPIO_BASE + 0x210) & BIT28) == 0;
  // The generic driver supplies nanoseconds. Cap a delayed callback to keep
  // debounce/hold timing bounded rather than interpreting a long gap as input.
  if (Delta > MS (100)) { Delta = MS (100); }
  if (Pressed != mSample) { mSample = Pressed; mDebounce = 0; }
  else { mDebounce += Delta; }
  if (mDebounce >= MS (20) && mStable != mSample) {
    mStable = mSample;
    if (mStable) { mHeld = 0; mLong = FALSE; }
    else if (!mLong) {
      Key.Key.ScanCode = SCAN_UP;
      Api->PushEfikeyBufTail (Api, &Key);
      DEBUG ((DEBUG_WARN, "DALI: keypad event scan=%x unicode=%x\n", Key.Key.ScanCode, Key.Key.UnicodeChar));
    }
  }
  if (mStable) {
    mHeld += Delta;
    if (!mLong && mHeld >= MS (700)) {
      Key.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
      Api->PushEfikeyBufTail (Api, &Key);
      DEBUG ((DEBUG_WARN, "DALI: keypad event scan=%x unicode=%x\n", Key.Key.ScanCode, Key.Key.UnicodeChar));
      mLong = TRUE;
    }
  }
  return EFI_SUCCESS;
}
