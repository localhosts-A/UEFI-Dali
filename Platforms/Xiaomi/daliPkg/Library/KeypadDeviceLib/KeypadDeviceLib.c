// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/TimerLib.h>
#include "KeypadTiming.h"
#include "PmicKeys.h"

// Official MT6991 pin ranges: GPIO60 DIN[28] at 0x210, DIR[28] at
// 0x10, MODE[19:16] at 0x370. Never change pinmux, pulls or direction.
STATIC BOOLEAN mReady;
STATIC DALI_KEY_STATE mKey, mPower, mDown;

RETURN_STATUS EFIAPI KeypadDeviceConstructor (VOID) {
  CONST VOID *Fdt = (VOID *)(UINTN)DALI_FDT_BASE;
  CONST UINT32 *Pins;
  INT32 Node, Controller, Length;
  UINT32 Mode, Direction;
  if (FdtCheckHeader (Fdt)) { return RETURN_SUCCESS; }
  DaliPmicKeysInitialize (Fdt);
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
  mKey = (DALI_KEY_STATE){0};
  mPower = (DALI_KEY_STATE){0};
  mDown = (DALI_KEY_STATE){0};
  return EFI_SUCCESS;
}

STATIC VOID PushKey (KEYPAD_RETURN_API *Api, UINT16 Scan, CHAR16 Unicode, UINT64 Now, CONST CHAR8 *Source) {
  EFI_KEY_DATA Key = {0};
  Key.Key.ScanCode = Scan;
  Key.Key.UnicodeChar = Unicode;
  Api->PushEfikeyBufTail (Api, &Key);
  DEBUG ((DEBUG_WARN, "DALI: keypad event scan=%x unicode=%x time-ms=%lu source=%a\n",
    Scan, Unicode, Now / 1000000, Source));
}

EFI_STATUS KeypadDeviceGetKeys (KEYPAD_DEVICE_PROTOCOL *This, KEYPAD_RETURN_API *Api, UINT64 Delta) {
  BOOLEAN Pressed;
  UINTN Event;
  UINT64 Now;
  UINT8 PmicRaw;
  Now = GetTimeInNanoSecond (GetPerformanceCounter ());
  if (!EFI_ERROR (DaliPmicKeysPoll (Now, &PmicRaw))) {
    // Run7 physical calibration: idle 0x1e, Volume down 0x16, Power 0x1c.
    // Keep Volume up on GPIO60 rather than emitting its PMIC bit twice.
    if (DaliKeyPressSample (&mDown, (PmicRaw & BIT3) == 0, Now)) {
      PushKey (Api, SCAN_DOWN, 0, Now, "volume-down");
    }
    if (DaliKeyPressSample (&mPower, (PmicRaw & BIT1) == 0, Now)) {
      PushKey (Api, SCAN_NULL, CHAR_CARRIAGE_RETURN, Now, "power");
    }
  }
  if (!mReady) { return EFI_SUCCESS; }
  Pressed = (MmioRead32 (DALI_GPIO_BASE + 0x210) & BIT28) == 0;
  Event = DaliKeySample (&mKey, Pressed, Now);
  if (Event != 0) {
    PushKey (Api, Event == DALI_KEY_UP ? SCAN_UP : SCAN_NULL,
      Event == DALI_KEY_ENTER ? CHAR_CARRIAGE_RETURN : 0, Now, "volume-up");
  }
  return EFI_SUCCESS;
}
