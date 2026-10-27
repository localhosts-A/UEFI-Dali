// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Read-only inventory, NOT a controller takeover or DMA qualification. MMIO
// offsets: pinned MiCode Linux f089d58 include/ufs/ufshci.h. Clock gates:
// device_modules e45c319 clk-mt6991-peri.c:616-693 and clk-gate.c:22-46.
#define DALI_UFS_HCI 0x16810000ULL
#define DALI_UFS_AO  0x168A0000ULL

typedef struct {
  UINT32 Gate0, Gate1, Cap, Version, Hce, Hcs, Is, Ie, Ahit;
  UINT32 UtrlLo, UtrlHi, UtrlDoorbell, UtrlRun;
  UINT32 UtmrlLo, UtmrlHi, UtmrlDoorbell, UtmrlRun;
  UINT32 McqCap, MemConfig, McqConfig;
  UINT32 IommusBytes;
  UINT32 MtkIpVersion, RefClock, XoUfs, Options, Outstanding;
  BOOLEAN HciRead, McqRead;
} DALI_UFS_SNAPSHOT;

typedef UINT32 (*DALI_UFS_READ32) (UINTN Address, VOID *Context);
typedef enum {
  DaliUfsProbeOff, DaliUfsProbeRejected, DaliUfsProbeGated,
  DaliUfsProbeInvalidMmio, DaliUfsProbeCaptured
} DALI_UFS_PROBE_RESULT;

STATIC BOOLEAN DaliUfsToken (CONST CHAR8 *Token) {
  if (Token == NULL) { return FALSE; }
  for (UINTN I = 0; I < 32; I++) {
    if (!((Token[I] >= '0' && Token[I] <= '9') || (Token[I] >= 'a' && Token[I] <= 'f'))) { return FALSE; }
  }
  return Token[32] == 0;
}

STATIC BOOLEAN DaliUfsNode (CONST VOID *Fdt, INT32 Node, INT32 Parent,
                          CONST CHAR8 *Compatible, UINT32 Base, UINT32 Size) {
  INT32 Length;
  CONST UINT32 *Reg;
  CONST CHAR8 *Status;
  if (Node < 0 || FdtParentOffset (Fdt, Node) != Parent ||
      FdtAddressCells (Fdt, Parent) != 2 || FdtSizeCells (Fdt, Parent) != 2 ||
      FdtStringListSearch (Fdt, Node, "compatible", Compatible) < 0) { return FALSE; }
  Status = FdtGetProp (Fdt, Node, "status", &Length);
  if (Status != NULL && !((Length == 5 && CompareMem (Status, "okay", 5) == 0) ||
                          (Length == 3 && CompareMem (Status, "ok", 3) == 0))) { return FALSE; }
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  return Reg != NULL && Length == 16 && Fdt32ToCpu (Reg[0]) == 0 &&
         Fdt32ToCpu (Reg[1]) == Base && Fdt32ToCpu (Reg[2]) == 0 && Fdt32ToCpu (Reg[3]) == Size;
}

STATIC BOOLEAN DaliUfsResources (CONST VOID *Fdt, UINT32 *IommusBytes) {
  INT32 Soc, Hci, Phy, Ao, Cksys, Length;
  CONST UINT32 *Property;
  CONST CHAR8 *Names;
  STATIC CONST CHAR8 ExpectedNames[] =
    "ufs_sel\0ufs_sel_min_src\0ufs_sel_max_src\0ufs_fde\0ufs_fde_min_src\0ufs_fde_max_src\0"
    "unipro_tx_sym\0unipro_rx_sym0\0unipro_rx_sym1\0unipro_sys\0unipro_sap\0ufs_phy_sap\0ufshci_ufs\0ufshci_aes";
  if (Fdt == NULL || FdtCheckHeader (Fdt) || FdtTotalSize (Fdt) > 0x100000 ||
      FdtStringListSearch (Fdt, 0, "compatible", "mediatek,MT6991") < 0) { return FALSE; }
  Soc = FdtPathOffset (Fdt, "/soc");
  if (Soc < 0 || FdtParentOffset (Fdt, Soc) != 0) { return FALSE; }
  Property = FdtGetProp (Fdt, Soc, "ranges", &Length);
  if (Property == NULL || Length != 0) { return FALSE; }
  Hci = FdtPathOffset (Fdt, "/soc/ufshci@16810000");
  Phy = FdtPathOffset (Fdt, "/soc/ufsphy@16800000");
  Ao = FdtPathOffset (Fdt, "/syscon@168a0000");
  Cksys = FdtPathOffset (Fdt, "/syscon@10000000");
  if (!DaliUfsNode (Fdt, Hci, Soc, "mediatek,mt8183-ufshci", DALI_UFS_HCI, 0x2A00) ||
      !DaliUfsNode (Fdt, Phy, Soc, "mediatek,mt8183-ufsphy", 0x16800000, 0x10000) ||
      !DaliUfsNode (Fdt, Ao, 0, "mediatek,mt6991-ufscfg_ao", DALI_UFS_AO, 0x1000) ||
      !DaliUfsNode (Fdt, Cksys, 0, "mediatek,mt6991-cksys", 0x10000000, 0x800)) { return FALSE; }
  Property = FdtGetProp (Fdt, Hci, "phys", &Length);
  if (Property == NULL || Length != 4 || FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Property)) != Phy) { return FALSE; }
  Property = FdtGetProp (Fdt, Phy, "#phy-cells", &Length);
  if (Property == NULL || Length != 4 || Fdt32ToCpu (*Property) != 0) { return FALSE; }
  Property = FdtGetProp (Fdt, Phy, "mphy-ver", &Length);
  if (Property == NULL || Length != 4 || Fdt32ToCpu (*Property) != 1) { return FALSE; }
  Property = FdtGetProp (Fdt, Ao, "#clock-cells", &Length);
  if (Property == NULL || Length != 4 || Fdt32ToCpu (*Property) != 1) { return FALSE; }
  Property = FdtGetProp (Fdt, Cksys, "#clock-cells", &Length);
  if (Property == NULL || Length != 4 || Fdt32ToCpu (*Property) != 1) { return FALSE; }
  Names = FdtGetProp (Fdt, Hci, "clock-names", &Length);
  if (Names == NULL || Length != sizeof ExpectedNames || CompareMem (Names, ExpectedNames, sizeof ExpectedNames)) { return FALSE; }
  Property = FdtGetProp (Fdt, Hci, "clocks", &Length);
  if (Property == NULL || Length != 28 * 4) { return FALSE; }
  STATIC CONST UINT32 ParentIds[] = {42, 97, 115, 41, 118, 96};
  for (UINTN I = 0; I < 6; I++) {
    if (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Property[I * 2])) != Cksys ||
        Fdt32ToCpu (Property[I * 2 + 1]) != ParentIds[I]) { return FALSE; }
  }
  // The eight named AO consumers are virtual aliases of the physical set/clr
  // gates. Pin the provider and IDs before interpreting its two status words.
  for (UINTN I = 0; I < 8; I++) {
    if (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Property[(I + 6) * 2])) != Ao ||
        Fdt32ToCpu (Property[(I + 6) * 2 + 1]) != I * 2 + 1) { return FALSE; }
  }
  Property = FdtGetProp (Fdt, Hci, "iommus", &Length);
  *IommusBytes = Property == NULL ? 0 : (UINT32)Length;
  return TRUE;
}

STATIC DALI_UFS_PROBE_RESULT DaliUfsSnapshot (
  CONST VOID *Fdt, BOOLEAN Enabled, CONST CHAR8 *Token, DALI_UFS_READ32 Read,
  VOID *Context, DALI_UFS_SNAPSHOT *Out
  ) {
  if (Out == NULL) { return DaliUfsProbeRejected; }
  ZeroMem (Out, sizeof *Out);
  if (!Enabled) { return DaliUfsProbeOff; }
  if (Read == NULL || !DaliUfsToken (Token) || !DaliUfsResources (Fdt, &Out->IommusBytes)) {
    return DaliUfsProbeRejected;
  }
  Out->Gate0 = Read (DALI_UFS_AO + 0x104, Context);
  Out->Gate1 = Read (DALI_UFS_AO + 0x004, Context);
  if (Out->Gate0 == 0xFFFFFFFFU || Out->Gate1 == 0xFFFFFFFFU) { return DaliUfsProbeInvalidMmio; }
  // Conservative register-access gate: HCI, UniPro system and SAP clocks must
  // already be enabled. Do not turn on gates or infer DMA ownership from them.
  if ((Out->Gate0 & 1) || (Out->Gate1 & 0x18)) { return DaliUfsProbeGated; }
  Out->Cap = Read (DALI_UFS_HCI + 0x00, Context);
  Out->Version = Read (DALI_UFS_HCI + 0x08, Context);
  if (Out->Cap == 0xFFFFFFFFU || Out->Version == 0 || Out->Version == 0xFFFFFFFFU) {
    return DaliUfsProbeInvalidMmio;
  }
  Out->HciRead = TRUE;
  Out->Hce = Read (DALI_UFS_HCI + 0x34, Context);
  Out->Hcs = Read (DALI_UFS_HCI + 0x30, Context);
  Out->Is = Read (DALI_UFS_HCI + 0x20, Context);
  Out->Ie = Read (DALI_UFS_HCI + 0x24, Context);
  Out->Ahit = Read (DALI_UFS_HCI + 0x18, Context);
  Out->UtrlLo = Read (DALI_UFS_HCI + 0x50, Context);
  Out->UtrlHi = Read (DALI_UFS_HCI + 0x54, Context);
  Out->UtrlDoorbell = Read (DALI_UFS_HCI + 0x58, Context);
  Out->UtrlRun = Read (DALI_UFS_HCI + 0x60, Context);
  Out->UtmrlLo = Read (DALI_UFS_HCI + 0x70, Context);
  Out->UtmrlHi = Read (DALI_UFS_HCI + 0x74, Context);
  Out->UtmrlDoorbell = Read (DALI_UFS_HCI + 0x78, Context);
  Out->UtmrlRun = Read (DALI_UFS_HCI + 0x80, Context);
  if ((Out->Cap & 0x40000000U) && Out->Version >= 0x400) {
    Out->McqCap = Read (DALI_UFS_HCI + 0x04, Context);
    Out->MemConfig = Read (DALI_UFS_HCI + 0x300, Context);
    Out->McqConfig = Read (DALI_UFS_HCI + 0x380, Context);
    Out->McqRead = TRUE;
  }
  // Pinned device_modules e45c319 drivers/ufs/ufs-mediatek.h:47-63.
  // Version identifies the MT6991 quirk branch; reference clock and vendor
  // state must be observed before initialization. These reads perform no UIC,
  // clock/power/reset writes, queue setup or secure-monitor calls.
  Out->MtkIpVersion = Read (DALI_UFS_HCI + 0x2240, Context);
  Out->RefClock = Read (DALI_UFS_HCI + 0x144, Context);
  Out->XoUfs = Read (DALI_UFS_HCI + 0x140, Context);
  Out->Options = Read (DALI_UFS_HCI + 0x160, Context);
  Out->Outstanding = Read (DALI_UFS_HCI + 0x14C, Context);
  // Captured only means reads completed. Active MCQ/doorbells, a disabled HCE,
  // or any particular address never establish safe takeover/direct DMA.
  return DaliUfsProbeCaptured;
}
