// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbPlatformLib.h>
#include "UsbFdt.h"
STATIC BOOLEAN Absent (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name) {
  INT32 Length;
  return FdtGetProp (Fdt, Node, Name, &Length) == NULL && Length == -FDT_ERR_NOTFOUND;
}
STATIC BOOLEAN ValueOrAbsent (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 Value) {
  return Absent (Fdt, Node, Name) || Cell (Fdt, Node, Name, Value);
}
STATIC BOOLEAN OnlyProperties (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *CONST *Names, UINTN Count) {
  UINTN Present = 0;
  for (UINTN I = 0; I < Count; ++I) {
    INT32 Length;
    if (FdtGetProp (Fdt, Node, Names[I], &Length) != NULL) { Present++; }
    else if (Length != -FDT_ERR_NOTFOUND) { return FALSE; }
  }
  INT32 Property = FdtFirstPropertyOffset (Fdt, Node);
  while (Property >= 0) {
    if (Present == 0) { return FALSE; }
    Present--; Property = FdtNextPropertyOffset (Fdt, Property);
  }
  return Property == -FDT_ERR_NOTFOUND && Present == 0;
}
STATIC BOOLEAN PinState (CONST VOID *Fdt, INT32 Nxp, INT32 Pio, CONST CHAR8 *Property,
  CONST CHAR8 *Path, CONST CHAR8 *Level, CONST CHAR8 *Opposite) {
  INT32 Length, State = FdtPathOffset (Fdt, Path);
  CONST UINT32 *Ref = FdtGetProp (Fdt, Nxp, Property, &Length);
  CONST CHAR8 *StateProperties[] = {"phandle", "linux,phandle", "status"};
  if (State < 0 || !Enabled (Fdt, State) || FdtParentOffset (Fdt, State) != Pio ||
      Ref == NULL || Length != 4 || UniqueHandle (Fdt, Fdt32ToCpu (*Ref)) != State ||
      !OnlyProperties (Fdt, State, StateProperties, 3)) { return FALSE; }
  // Exactly the one output group; do not silently accept another pin action.
  INT32 Child = FdtNextNode (Fdt, State, NULL);
  CONST CHAR8 *GroupProperties[] = {"pinmux", Level, "phandle", "linux,phandle", "status"};
  if (Child < 0 || FdtParentOffset (Fdt, Child) != State || !Enabled (Fdt, Child) ||
      !Cell (Fdt, Child, "pinmux", 72U << 8) || !Empty (Fdt, Child, Level) ||
      !Absent (Fdt, Child, Opposite) || !OnlyProperties (Fdt, Child, GroupProperties, 5)) { return FALSE; }
  INT32 Next = FdtNextNode (Fdt, Child, NULL);
  return Next == -FDT_ERR_NOTFOUND || (Next >= 0 && FdtParentOffset (Fdt, Next) != State &&
    FdtParentOffset (Fdt, Next) != Child);
}
EFI_STATUS DaliUsbReadI2cResources (CONST VOID *Fdt, UINTN FdtSize, DALI_USB_I2C_RESOURCES *Out) {
  if (Out == NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_CLOCK_RESOURCES Clocks;
  EFI_STATUS Status = DaliUsbReadClockResources (Fdt, FdtSize, &Clocks);
  if (EFI_ERROR (Status)) { return Status; }
  INT32 Soc = FdtPathOffset (Fdt, "/soc"), Pio = FdtPathOffset (Fdt, "/soc/pinctrl@1002d000");
  CONST UINT32 Bases[] = {0x1002d000,0x12000000,0x12020000,0x12040000,0x12060000,0x12820000,
    0x12840000,0x12860000,0x13000000,0x13020000,0x13040000,0x130f0000,0x13110000,
    0x13800000,0x13820000,0x13860000};
  CONST CHAR8 *Names[] = {"gpio","iocfg_rt","iocfg_rm1","iocfg_rm2","iocfg_rb","iocfg_bm1",
    "iocfg_bm2","iocfg_bm3","iocfg_lt","iocfg_lm1","iocfg_lm2","iocfg_lb1","iocfg_lb2",
    "iocfg_tm1","iocfg_tm2","iocfg_tm3"};
  INT32 Length;
  CONST UINT32 *Reg = FdtGetProp (Fdt, Pio, "reg", &Length);
  if (!Compatible (Fdt, Pio, "mediatek,mt6991-pinctrl") || FdtParentOffset (Fdt, Pio) != Soc ||
      !Empty (Fdt, Pio, "gpio-controller") || !Cell (Fdt, Pio, "#gpio-cells", 2) ||
      Reg == NULL || Length != 16 * 16 || FdtStringListCount (Fdt, Pio, "reg-names") != 16) { return EFI_UNSUPPORTED; }
  for (UINTN I = 0; I < 16; ++I) {
    if (Fdt32ToCpu (Reg[4 * I]) != 0 || Fdt32ToCpu (Reg[4 * I + 1]) != Bases[I] ||
        Fdt32ToCpu (Reg[4 * I + 2]) != 0 || Fdt32ToCpu (Reg[4 * I + 3]) != 0x1000 ||
        FdtStringListSearch (Fdt, Pio, "reg-names", Names[I]) != (INT32)I) { return EFI_UNSUPPORTED; }
  }
  CONST UINT32 *Ranges = FdtGetProp (Fdt, Pio, "gpio-ranges", &Length);
  if (Ranges == NULL || Length != 16 || UniqueHandle (Fdt, Fdt32ToCpu (Ranges[0])) != Pio ||
      Fdt32ToCpu (Ranges[1]) != 0 || Fdt32ToCpu (Ranges[2]) != 0 || Fdt32ToCpu (Ranges[3]) != 271) { return EFI_UNSUPPORTED; }
  INT32 I2c = FdtPathOffset (Fdt, "/soc/i2c@13170000");
  if (!Cell (Fdt, I2c, "#address-cells", 1) || !Cell (Fdt, I2c, "#size-cells", 0) ||
      !Cell (Fdt, I2c, "scl-gpio-id", 104) || !Cell (Fdt, I2c, "sda-gpio-id", 105) ||
      !ValueOrAbsent (Fdt, I2c, "ch-offset-i2c", 0) || !ValueOrAbsent (Fdt, I2c, "ch-offset-dma", 0) ||
      !ValueOrAbsent (Fdt, I2c, "i2c-offset-ap", 0x100)) { return EFI_UNSUPPORTED; }
  CONST CHAR8 *Unsupported[] = {"mediatek,use-push-pull", "mediatek,disable-clk-stretch",
    "mediatek,have-pmic", "mediatek,control-irq-sel", "mediatek,control-rs-stop"};
  for (UINTN I = 0; I < sizeof Unsupported / sizeof Unsupported[0]; ++I) {
    if (!Absent (Fdt, I2c, Unsupported[I])) { return EFI_UNSUPPORTED; }
  }
  INT32 Nxp = FdtPathOffset (Fdt, "/soc/i2c@13170000/nxp-eusb2-repeater@4f");
  if (!Compatible (Fdt, Nxp, "mtk,nxp-eusb2-repeater") || FdtParentOffset (Fdt, Nxp) != I2c ||
      !Cell (Fdt, Nxp, "reg", 0x4f) || !Cell (Fdt, Nxp, "#phy-cells", 0) ||
      FdtStringListCount (Fdt, Nxp, "pinctrl-names") != 2 ||
      FdtStringListSearch (Fdt, Nxp, "pinctrl-names", "enable") != 0 ||
      FdtStringListSearch (Fdt, Nxp, "pinctrl-names", "disable") != 1 ||
      !PinState (Fdt, Nxp, Pio, "pinctrl-0", "/soc/pinctrl@1002d000/rptr-enable", "output-high", "output-low") ||
      !PinState (Fdt, Nxp, Pio, "pinctrl-1", "/soc/pinctrl@1002d000/rptr-disable", "output-low", "output-high")) { return EFI_UNSUPPORTED; }
  DALI_USB_I2C_RESOURCES Value = {{0x1002d000,0x1000},{0x13110000,0x1000},
    {0x13170000,0x20000},{0x16410000,0x10000},104,105,72};
  *Out = Value; return EFI_SUCCESS;
}
