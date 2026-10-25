// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbPlatformLib.h>
#include "UsbFdt.h"

STATIC BOOLEAN Names (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Property,
  CONST CHAR8 *CONST *Expected, UINTN Count) {
  if (FdtStringListCount (Fdt, Node, Property) != (INT32)Count) { return FALSE; }
  for (UINTN I = 0; I < Count; ++I) {
    if (FdtStringListSearch (Fdt, Node, Property, Expected[I]) != (INT32)I) { return FALSE; }
  }
  return TRUE;
}
STATIC BOOLEAN Region (CONST VOID *Fdt, INT32 Node, UINT32 Base, UINT32 Size) {
  CONST UINT32 Reg[] = {0, Base, 0, Size};
  return Words (Fdt, Node, "reg", Reg, 4);
}
STATIC BOOLEAN FixedRef (CONST VOID *Fdt, INT32 Consumer, INT32 Provider) {
  INT32 Length;
  CONST UINT32 *P = FdtGetProp (Fdt, Consumer, "clocks", &Length);
  return P != NULL && Length == 4 && UniqueHandle (Fdt, Fdt32ToCpu (*P)) == Provider;
}
STATIC BOOLEAN IndexedClocks (CONST VOID *Fdt, INT32 Consumer, CONST INT32 *Providers,
  CONST UINT32 *Ids, UINTN Count) {
  INT32 Length;
  CONST UINT32 *P = FdtGetProp (Fdt, Consumer, "clocks", &Length);
  if (P == NULL || Length != (INT32)(Count * 8)) { return FALSE; }
  for (UINTN I = 0; I < Count; ++I) {
    if (UniqueHandle (Fdt, Fdt32ToCpu (P[2 * I])) != Providers[I] ||
        Fdt32ToCpu (P[2 * I + 1]) != Ids[I]) { return FALSE; }
  }
  return TRUE;
}
EFI_STATUS DaliUsbReadClockResources (CONST VOID *Fdt, UINTN FdtSize, DALI_USB_CLOCK_RESOURCES *Out) {
  if (Out == NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_CLOCK_RESOURCES Value = {0};
  /* This also validates full structure, enabled root/SOC, identity translation
   * and the exact USB0/PHY/calibration graph before any traversal below. */
  EFI_STATUS Status = DaliUsbReadCalibration (Fdt, FdtSize, &Value.Calibration);
  if (Status != EFI_SUCCESS) { return Status; }
  CONST CHAR8 *Paths[] = {"/syscon@1c016000", "/syscon@10000000", "/syscon@10000800",
    "/syscon@131b0000", "/syscon@16640000"};
  CONST CHAR8 *Compat[] = {"mediatek,mt6991-vlp_cksys", "mediatek,mt6991-cksys",
    "mediatek,mt6991-apmixedsys", "mediatek,mt6991-imp_iic_wrap_w", "mediatek,mt6991-pericfg_ao"};
  CONST DALI_USB_MMIO_RANGE Windows[] = {{0x1c016000, 0x1000}, {0x10000000, 0x800},
    {0x10000800, 0x1000}, {0x131b0000, 0x1000}, {0x16640000, 0x1000}};
  INT32 Nodes[DaliUsbClockBankCount];
  for (UINTN I = 0; I < DaliUsbClockBankCount; ++I) {
    Nodes[I] = FdtPathOffset (Fdt, Paths[I]);
    if (!Compatible (Fdt, Nodes[I], Compat[I]) || !Compatible (Fdt, Nodes[I], "syscon") ||
        FdtParentOffset (Fdt, Nodes[I]) != 0 || !Cell (Fdt, Nodes[I], "#clock-cells", 1) ||
        !Region (Fdt, Nodes[I], Windows[I].Base, Windows[I].Size)) { return EFI_UNSUPPORTED; }
    Value.Bank[I] = Windows[I];
  }
  INT32 Clocks = FdtPathOffset (Fdt, "/clocks"), Ref = FdtPathOffset (Fdt, "/clocks/clk26m");
  if (Clocks < 0 || !Enabled (Fdt, Clocks) || FdtParentOffset (Fdt, Clocks) != 0 ||
      !Compatible (Fdt, Ref, "fixed-clock") || FdtParentOffset (Fdt, Ref) != Clocks ||
      !Cell (Fdt, Ref, "#clock-cells", 0) || !Cell (Fdt, Ref, "clock-frequency", 26000000)) { return EFI_UNSUPPORTED; }
  INT32 Usb = FdtPathOffset (Fdt, "/soc/usb0@16701000");
  INT32 Phy = FdtPathOffset (Fdt, "/soc/usb-phy0@16730000/usb2-phy0@16730000");
  INT32 Host = FdtPathOffset (Fdt, "/soc/usb0@16701000/xhci0@16700000");
  INT32 I2c = FdtPathOffset (Fdt, "/soc/i2c@13170000");
  CONST UINT32 UsbReg[] = {0, 0x16701000, 0, 0x2e00, 0, 0x16703e00, 0, 0x100};
  CONST UINT32 I2cReg[] = {0, 0x13170000, 0, 0x20000, 0, 0x16410000, 0, 0x10000};
  CONST UINT32 UsbIds[] = {34, 35, 36}, I2cIds[] = {5, 42};
  CONST INT32 UsbProviders[] = {Nodes[DaliUsbClockVlp], Nodes[DaliUsbClockVlp], Nodes[DaliUsbClockTop]};
  CONST INT32 I2cProviders[] = {Nodes[DaliUsbClockImpw], Nodes[DaliUsbClockPerao]};
  CONST CHAR8 *UsbNames[] = {"sys_ck", "host_ck", "frmcnt_ck"}, *I2cNames[] = {"main", "dma"};
  CONST CHAR8 *RegNames[] = {"mac", "ippc"}, *Mac[] = {"mac"}, *RefName[] = {"ref"};
  CONST CHAR8 *Sys[] = {"sys_ck"}, *Speed[] = {"high-speed"}, *Mode[] = {"otg"};
  if (!Words (Fdt, Usb, "reg", UsbReg, 8) || !Names (Fdt, Usb, "reg-names", RegNames, 2) ||
      !Cell (Fdt, Usb, "#address-cells", 2) || !Cell (Fdt, Usb, "#size-cells", 2) || !Empty (Fdt, Usb, "ranges") ||
      !Names (Fdt, Usb, "maximum-speed", Speed, 1) || !Names (Fdt, Usb, "dr_mode", Mode, 1) ||
      !Empty (Fdt, Usb, "usb-role-switch") || !Empty (Fdt, Usb, "mediatek,clk-mgr") ||
      !Names (Fdt, Usb, "clock-names", UsbNames, 3) || !IndexedClocks (Fdt, Usb, UsbProviders, UsbIds, 3) ||
      !Names (Fdt, Phy, "clock-names", RefName, 1) || !FixedRef (Fdt, Phy, Ref) ||
      !Compatible (Fdt, Host, "mediatek,mtk-xhci") || FdtParentOffset (Fdt, Host) != Usb ||
      !Region (Fdt, Host, 0x16700000, 0x1000) || !Names (Fdt, Host, "reg-names", Mac, 1) ||
      !Names (Fdt, Host, "clock-names", Sys, 1) || !FixedRef (Fdt, Host, Ref) ||
      !Compatible (Fdt, I2c, "mediatek,mt6991-i2c") || FdtParentOffset (Fdt, I2c) != FdtPathOffset (Fdt, "/soc") ||
      !Words (Fdt, I2c, "reg", I2cReg, 8) || !Names (Fdt, I2c, "clock-names", I2cNames, 2) ||
      !IndexedClocks (Fdt, I2c, I2cProviders, I2cIds, 2) || !Cell (Fdt, I2c, "clock-div", 1) ||
      !Cell (Fdt, I2c, "clock-frequency", 400000)) { return EFI_UNSUPPORTED; }
  Value.ReferenceHz = 26000000;
  *Out = Value;
  return EFI_SUCCESS;
}

EFI_STATUS DaliUsbReadXhciResources (CONST VOID *Fdt, UINTN FdtSize, DALI_USB_XHCI_RESOURCES *Out) {
  if (Out == NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_CLOCK_RESOURCES Validated;
  EFI_STATUS Status = DaliUsbReadClockResources (Fdt, FdtSize, &Validated);
  if (Status != EFI_SUCCESS) { return Status; }
  DALI_USB_XHCI_RESOURCES Value = {{0x16700000, 0x1000}};
  *Out = Value;
  return EFI_SUCCESS;
}
