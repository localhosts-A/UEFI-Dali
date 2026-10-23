// SPDX-License-Identifier: BSD-2-Clause-Patent
// MT6991 PMIF SWINF2 read-only PMIC transaction, using the official vendor
// spmi-mtk-pmif-core.c register contract. Preserve inherited clocks and routes.
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/TimerLib.h>
#include "PmicKeys.h"

#define PMIF_ACC      0x880
#define PMIF_RDATA    0x894
#define PMIF_VLD_CLR  0x8A4
#define PMIF_STA      0x8A8
STATIC BOOLEAN mReady, mHaveRaw;
STATIC UINT8 mLastRaw;
STATIC UINT64 mNextPoll;
STATIC UINTN mChanges;

STATIC EFI_STATUS ReadTopStatus (UINT8 *Value) {
  UINT32 State = MmioRead32 (DALI_PMIF_BASE + PMIF_STA);
  // Never consume or clear somebody else's pending transaction.
  if (((State >> 1) & 7) != 0) { return EFI_NOT_READY; }
  // EXT_REG_LONG read, SID4, one byte, MT6363 TOPSTATUS 0x1e. BIT29 (write)
  // is deliberately zero; no PMIC register writes are implemented here.
  MmioWrite32 (DALI_PMIF_BASE + PMIF_ACC, (3U << 30) | (4U << 24) | 0x1E);
  for (UINTN I = 0; I < 1000; I++) {
    State = MmioRead32 (DALI_PMIF_BASE + PMIF_STA);
    if (((State >> 1) & 7) == 6) {
      *Value = (UINT8)MmioRead32 (DALI_PMIF_BASE + PMIF_RDATA);
      MmioWrite32 (DALI_PMIF_BASE + PMIF_VLD_CLR, 1);
      return (State & BIT18) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

VOID DaliPmicKeysInitialize (CONST VOID *Fdt) {
  INT32 Node, Length;
  CONST UINT32 *Reg, *Mask;
  CONST UINT32 Expected[] = {0, 0x1C01A000, 0, 0x8FF, 0, 0x1C01C000, 0, 0x100,
                            0, 0x1C018000, 0, 0x8FF, 0, 0x1C01C800, 0, 0x100};
  CONST CHAR8 *KeyPaths[] = {"/spmi@1c01a000/pmic@4/mt6363keys/power",
                            "/spmi@1c01a000/pmic@4/mt6363keys/home"};
  CONST UINT32 KeyCodes[] = {116, 114};
  Node = FdtPathOffset (Fdt, "/spmi@1c01a000");
  if (Node < 0 || FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6991-spmi") < 0) { return; }
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Reg == NULL || Length != sizeof Expected) { return; }
  for (UINTN I = 0; I < ARRAY_SIZE (Expected); I++) {
    if (Fdt32ToCpu (Reg[I]) != Expected[I]) { return; }
  }
  Mask = FdtGetProp (Fdt, Node, "spmi-dev-mask", &Length);
  if (Mask == NULL || Length != 4 || (Fdt32ToCpu (*Mask) & BIT4)) { return; }
  Node = FdtPathOffset (Fdt, "/spmi@1c01a000/pmic@4");
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Node < 0 || Reg == NULL || Length != 8 || Fdt32ToCpu (Reg[0]) != 4 || Reg[1] != 0 ||
      FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6363") < 0) { return; }
  Node = FdtPathOffset (Fdt, "/spmi@1c01a000/pmic@4/mt6363keys");
  if (Node < 0 || FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6363-keys") < 0) { return; }
  for (UINTN I = 0; I < ARRAY_SIZE (KeyPaths); I++) {
    Node = FdtPathOffset (Fdt, KeyPaths[I]);
    Reg = FdtGetProp (Fdt, Node, "linux,keycodes", &Length);
    if (Node < 0 || Reg == NULL || Length != 4 || Fdt32ToCpu (*Reg) != KeyCodes[I]) { return; }
  }
  DEBUG ((DEBUG_WARN, "DALI: PMIF probe init=%x SWINF2=%x SID4 path=M\n",
    MmioRead32 (DALI_PMIF_BASE), MmioRead32 (DALI_PMIF_BASE + PMIF_STA)));
  if (!(MmioRead32 (DALI_PMIF_BASE) & BIT0)) { return; }
  mReady = TRUE;
}

EFI_STATUS DaliPmicKeysPoll (UINT64 Now, UINT8 *Raw) {
  EFI_STATUS Status;
  if (!mReady || Now < mNextPoll) { return EFI_NOT_READY; }
  mNextPoll = Now + 20000000ULL;
  Status = ReadTopStatus (Raw);
  if (EFI_ERROR (Status)) {
    mReady = FALSE;
    DEBUG ((DEBUG_WARN, "DALI: PMIC key sampling disabled status=%r SWINF2=%x\n", Status,
      MmioRead32 (DALI_PMIF_BASE + PMIF_STA)));
    return Status;
  }
  if (!mHaveRaw || *Raw != mLastRaw) {
    if (mChanges++ < 64) {
      DEBUG ((DEBUG_WARN, "DALI: MT6363 TOPSTATUS=%x time-ms=%lu\n", *Raw, Now / 1000000));
    }
    mLastRaw = *Raw;
    mHaveRaw = TRUE;
  }
  return EFI_SUCCESS;
}
