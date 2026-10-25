// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbPlatformLib.h>
#include "UsbFdt.h"

STATIC BOOLEAN String (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST CHAR8 *Value) {
  return FdtStringListCount (Fdt, Node, Name) == 1 && FdtStringListSearch (Fdt, Node, Name, Value) == 0;
}
EFI_STATUS DaliUsbReadVusbResources (CONST VOID *Fdt, UINTN FdtSize, DALI_USB_VUSB_RESOURCES *Out) {
  if (Out == NULL) { return EFI_INVALID_PARAMETER; }
  struct dali_xsphy_calibration Calibration;
  EFI_STATUS Status = DaliUsbReadCalibration (Fdt, FdtSize, &Calibration);
  if (EFI_ERROR (Status)) { return Status; }
  INT32 Pmif = FdtPathOffset (Fdt, "/spmi@1c01a000");
  CONST UINT32 Regs[] = {0,0x1c01a000,0,0x8ff, 0,0x1c01c000,0,0x100,
    0,0x1c018000,0,0x8ff, 0,0x1c01c800,0,0x100};
  CONST CHAR8 *Names[] = {"pmif", "spmimst", "pmif-p", "spmimst-p"};
  if (!Compatible (Fdt, Pmif, "mediatek,mt6991-spmi") || FdtParentOffset (Fdt, Pmif) != 0 ||
      !Cell (Fdt, Pmif, "#address-cells", 2) || !Cell (Fdt, Pmif, "#size-cells", 0) ||
      !Words (Fdt, Pmif, "reg", Regs, 16) || FdtStringListCount (Fdt, Pmif, "reg-names") != 4) { return EFI_UNSUPPORTED; }
  for (UINTN I = 0; I < 4; I++) {
    if (FdtStringListSearch (Fdt, Pmif, "reg-names", Names[I]) != (INT32)I) { return EFI_UNSUPPORTED; }
  }
  INT32 Length;
  CONST UINT32 *Route = FdtGetProp (Fdt, Pmif, "spmi-dev-mask", &Length);
  if (Route == NULL || Length != 4 || Fdt32ToCpu (*Route) > 0xffff || (Fdt32ToCpu (*Route) & (1U << 5))) { return EFI_UNSUPPORTED; }
  INT32 Pmic = FdtPathOffset (Fdt, "/spmi@1c01a000/second_pmic@5");
  CONST UINT32 Sid[] = {5,0}, Cid[] = {0x73,9,8}, Modes[] = {0,2};
  INT32 Debug = FdtPathOffset (Fdt, "/spmi@1c01a000/second_pmic@5/mtk-spmi-pmic-debug");
  if (!Compatible (Fdt, Pmic, "mediatek,mt6373") || FdtParentOffset (Fdt, Pmic) != Pmif ||
      !Words (Fdt, Pmic, "reg", Sid, 2) || !Compatible (Fdt, Debug, "mediatek,spmi-pmic-debug") ||
      FdtParentOffset (Fdt, Debug) != Pmic || !Words (Fdt, Debug, "cid-addr", Cid, 3)) { return EFI_UNSUPPORTED; }
  INT32 Regulators = FdtPathOffset (Fdt, "/spmi@1c01a000/second_pmic@5/regulators");
  INT32 Rail = FdtPathOffset (Fdt, "/spmi@1c01a000/second_pmic@5/regulators/vusb");
  if (Regulators < 0 || !Enabled (Fdt, Regulators) || FdtParentOffset (Fdt, Regulators) != Pmic ||
      Rail < 0 || !Enabled (Fdt, Rail) || FdtParentOffset (Fdt, Rail) != Regulators ||
      !String (Fdt, Rail, "regulator-compatible", "VUSB") || !String (Fdt, Rail, "regulator-name", "mt6373_vusb") ||
      !Cell (Fdt, Rail, "regulator-min-microvolt", 1200000) || !Cell (Fdt, Rail, "regulator-max-microvolt", 3500000) ||
      !Cell (Fdt, Rail, "regulator-enable-ramp-delay", 720) || !Words (Fdt, Rail, "regulator-allowed-modes", Modes, 2) ||
      !Empty (Fdt, Rail, "regulator-always-on")) { return EFI_UNSUPPORTED; }
  INT32 Usb = FdtPathOffset (Fdt, "/soc/usb0@16701000");
  CONST UINT32 *Supply = FdtGetProp (Fdt, Usb, "vusb33-supply", &Length);
  if (Supply == NULL || Length != 4 || UniqueHandle (Fdt, Fdt32ToCpu (*Supply)) != Rail) { return EFI_UNSUPPORTED; }
  DALI_USB_VUSB_RESOURCES Value = {(UINT16)Fdt32ToCpu (*Route), 1200000, 3500000, 720};
  *Out = Value; return EFI_SUCCESS;
}
