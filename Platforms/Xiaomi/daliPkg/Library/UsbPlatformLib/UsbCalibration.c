// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbPlatformLib.h>
#include "UsbFdt.h"
STATIC BOOLEAN ValidateCells (CONST VOID *Fdt, INT32 Phy) {
  CONST CHAR8 *Names[] = {"hsrx_term_cal", "hstx_impn", "hstx_impp"};
  CONST UINT32 Masks[] = {0xf00000, 0x7c00, 0xf8000};
  INT32 Length;
  CONST UINT32 *Refs = FdtGetProp (Fdt, Phy, "nvmem-cells", &Length);
  if (Refs == NULL || Length != 12 || Refs[1] != Refs[2] || Refs[0] == Refs[1] ||
      !Words (Fdt, Phy, "nvmem-cell-masks", Masks, 3) ||
      FdtStringListCount (Fdt, Phy, "nvmem-cell-names") != 3 || !Empty (Fdt, Phy, "mediatek,u2-sw-efuse")) { return FALSE; }
  INT32 Provider = -1;
  for (UINTN I = 0; I < 3; ++I) {
    if (FdtStringListSearch (Fdt, Phy, "nvmem-cell-names", Names[I]) != (INT32)I) { return FALSE; }
    INT32 Node = UniqueHandle (Fdt, Fdt32ToCpu (Refs[I]));
    if (Node < 0 || !Enabled (Fdt, Node)) { return FALSE; }
    CONST UINT32 Reg[] = {I == 0 ? 0x1d4u : 0x1d8u, 4};
    if (!Words (Fdt, Node, "reg", Reg, 2)) { return FALSE; }
    INT32 Parent = FdtParentOffset (Fdt, Node);
    if (!Compatible (Fdt, Parent, "mediatek,devinfo") || !Cell (Fdt, Parent, "#address-cells", 1) ||
        !Cell (Fdt, Parent, "#size-cells", 1) || FdtParentOffset (Fdt, Parent) != 0 ||
        (Provider >= 0 && Provider != Parent)) { return FALSE; }
    Provider = Parent;
  }
  return TRUE;
}
EFI_STATUS DaliUsbReadCalibration (CONST VOID *Fdt, UINTN FdtSize, struct dali_xsphy_calibration *Out) {
  if (Fdt == NULL || Out == NULL || ((UINTN)Fdt & 7) || FdtSize < 40 || FdtSize > 16 * 1024 * 1024) { return EFI_INVALID_PARAMETER; }
  if (fdt_check_full (Fdt, FdtSize) || FdtTotalSize (Fdt) < 40 || FdtTotalSize (Fdt) > FdtSize) { return EFI_COMPROMISED_DATA; }
  INT32 Root = FdtPathOffset (Fdt, "/"), Soc = FdtPathOffset (Fdt, "/soc");
  if (Root != 0 || !Compatible (Fdt, Root, "mediatek,MT6991") || !Cell (Fdt, Root, "#address-cells", 2) ||
      !Cell (Fdt, Root, "#size-cells", 2) || Soc < 0 || !Enabled (Fdt, Soc) ||
      !Cell (Fdt, Soc, "#address-cells", 2) || !Cell (Fdt, Soc, "#size-cells", 2) || !Empty (Fdt, Soc, "ranges")) { return EFI_UNSUPPORTED; }
  INT32 Usb = FdtPathOffset (Fdt, "/soc/usb0@16701000");
  INT32 Parent = FdtPathOffset (Fdt, "/soc/usb-phy0@16730000");
  INT32 Phy = FdtPathOffset (Fdt, "/soc/usb-phy0@16730000/usb2-phy0@16730000");
  CONST UINT32 PhyReg[] = {0, 0x16730000, 0, 0x400};
  if (!Compatible (Fdt, Usb, "mediatek,mt6991-mtu3") || !Compatible (Fdt, Parent, "mediatek,xsphy") ||
      Phy < 0 || !Enabled (Fdt, Phy) || FdtParentOffset (Fdt, Phy) != Parent ||
      !Cell (Fdt, Parent, "#address-cells", 2) || !Cell (Fdt, Parent, "#size-cells", 2) || !Empty (Fdt, Parent, "ranges") ||
      !Words (Fdt, Parent, "reg", PhyReg, 4) || !Words (Fdt, Phy, "reg", PhyReg, 4) || !Cell (Fdt, Phy, "#phy-cells", 1)) { return EFI_UNSUPPORTED; }
  INT32 Length;
  CONST UINT32 *Refs = FdtGetProp (Fdt, Usb, "phys", &Length);
  if (Refs == NULL || Length != 16 || Fdt32ToCpu (Refs[1]) != 3 || UniqueHandle (Fdt, Fdt32ToCpu (Refs[0])) != Phy ||
      !ValidateCells (Fdt, Phy)) { return EFI_UNSUPPORTED; }
  INT32 Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) { Chosen = FdtPathOffset (Fdt, "/chosen@0"); }
  if (Chosen < 0 || !Enabled (Fdt, Chosen)) { return EFI_UNSUPPORTED; }
  INT32 ChipSize, InfoSize;
  CONST VOID *Chip = FdtGetProp (Fdt, Chosen, "atag,chipid", &ChipSize);
  CONST VOID *Info = FdtGetProp (Fdt, Chosen, "atag,devinfo", &InfoSize);
  if (Chip == NULL || Info == NULL || ChipSize < 0 || InfoSize < 0) { return EFI_UNSUPPORTED; }
  int Result = dali_usb_calibration_decode (Chip, (UINTN)ChipSize, Info, (UINTN)InfoSize, Out);
  return Result == USB_OK ? EFI_SUCCESS : Result == USB_UNSUPPORTED ? EFI_UNSUPPORTED : EFI_COMPROMISED_DATA;
}
