"""Execute the actual read-only firmware probe with hostile FDT/MMIO fixtures."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


TOKEN = b"0123456789abcdef0123456789abcdef"


def tree(*, hci_size="0x2a00", clock_id=13, parent_id=42, parent_provider=2, status='status = "okay";', ranges="ranges;",
         phy_ref=3, phy_version=1, iommu="", names=None):
    names = names or ['ufs_sel', 'ufs_sel_min_src', 'ufs_sel_max_src', 'ufs_fde', 'ufs_fde_min_src',
                      'ufs_fde_max_src', 'unipro_tx_sym', 'unipro_rx_sym0', 'unipro_rx_sym1',
                      'unipro_sys', 'unipro_sap', 'ufs_phy_sap', 'ufshci_ufs', 'ufshci_aes']
    clocks = ' '.join(f'{parent_provider if i == 0 else 2} {parent_id if i == 0 else value}'
                      for i, value in enumerate((42, 97, 115, 41, 118, 96))) + ' ' + \
             ' '.join(f'1 {clock_id if i == 6 else i * 2 + 1}' for i in range(8))
    named = ','.join('"' + name + '"' for name in names)
    dts = f'''/dts-v1/; / {{ compatible="mediatek,MT6991"; #address-cells=<2>; #size-cells=<2>;
      syscon@168a0000 {{ compatible="mediatek,mt6991-ufscfg_ao"; reg=<0 0x168a0000 0 0x1000>;
        #clock-cells=<1>; phandle=<1>; }};
      syscon@10000000 {{ compatible="mediatek,mt6991-cksys"; reg=<0 0x10000000 0 0x800>;
        #clock-cells=<1>; phandle=<2>; }};
      soc {{ #address-cells=<2>; #size-cells=<2>; {ranges}
        ufsphy@16800000 {{ compatible="mediatek,mt8183-ufsphy"; reg=<0 0x16800000 0 0x10000>;
          #phy-cells=<0>; mphy-ver=<{phy_version}>; phandle=<3>; }};
        ufshci@16810000 {{ compatible="mediatek,mt8183-ufshci"; reg=<0 0x16810000 0 {hci_size}>;
          {status} phys=<{phy_ref}>; clocks=<{clocks}>; clock-names={named}; {iommu} }};
      }}; }};'''
    return subprocess.check_output(['dtc', '-q', '-I', 'dts', '-O', 'dtb'], input=dts.encode())


class UfsProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler or not shutil.which('dtc'):
            raise unittest.SkipTest('C compiler, dtc and libfdt required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        header = Path(__file__).resolve().parents[2] / 'Silicon/MediaTek/MT6991Pkg/Library/PlatformLib'
        (base / 'probe.c').write_text('''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <libfdt.h>
typedef uint32_t UINT32; typedef int32_t INT32; typedef size_t UINTN;
typedef char CHAR8; typedef bool BOOLEAN;
#define VOID void
#define STATIC static
#define CONST const
#define TRUE true
#define FALSE false
#define ZeroMem(p,n) memset(p,0,n)
#define CompareMem memcmp
#define FdtCheckHeader fdt_check_header
#define FdtTotalSize fdt_totalsize
#define FdtStringListSearch fdt_stringlist_search
#define FdtPathOffset fdt_path_offset
#define FdtParentOffset fdt_parent_offset
#define FdtAddressCells fdt_address_cells
#define FdtSizeCells fdt_size_cells
#define FdtGetProp fdt_getprop
#define Fdt32ToCpu fdt32_to_cpu
#define FdtNodeOffsetByPhandle fdt_node_offset_by_phandle
#include "UfsProbe.h"
typedef struct { const uint32_t *values; size_t count; uint64_t addresses[32]; } Port;
static uint32_t read32(size_t address, void *context) {
  Port *p=context;
  if (p->count>=32) return UINT32_MAX;
  p->addresses[p->count++]=address;
  if (address==DALI_UFS_AO+0x104) return p->values[0];
  if (address==DALI_UFS_AO+4) return p->values[1];
  if (address==DALI_UFS_HCI) return p->values[2];
  if (address==DALI_UFS_HCI+8) return p->values[3];
  return (uint32_t)address;
}
int probe(const void *fdt, int enabled, const char *token, const uint32_t *values,
          uint64_t *addresses, uint32_t *fields, size_t *count) {
  Port p={.values=values}; DALI_UFS_SNAPSHOT out;
  int result=DaliUfsSnapshot(fdt, enabled!=0, token, read32, &p, &out);
  memcpy(addresses,p.addresses,sizeof p.addresses);
  memcpy(fields,&out,sizeof out); *count=p.count;
  return result;
}
''')
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(header), str(base / 'probe.c'), '-lfdt', '-o', str(base / 'probe.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'probe.so'))
        cls.lib.probe.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p,
                                  ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint64),
                                  ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_size_t)]
        cls.lib.probe.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, data=None, enabled=True, token=TOKEN, gate0=0, gate1=0, cap=0x4000001f, version=0x400):
        data = tree() if data is None else data
        addresses, fields, count = (ctypes.c_uint64 * 32)(), (ctypes.c_uint32 * 32)(), ctypes.c_size_t()
        result = self.lib.probe(ctypes.create_string_buffer(data), enabled, token,
                                (ctypes.c_uint32 * 4)(gate0, gate1, cap, version),
                                addresses, fields, ctypes.byref(count))
        return result, list(addresses[:count.value]), list(fields)

    def test_disabled_probe_never_parses_or_reads_mmio(self):
        result, addresses, _ = self.probe(data=b'invalid', enabled=False, token=b'none')
        self.assertEqual((result, addresses), (0, []))

    def test_invalid_identity_and_resources_never_read_any_mmio(self):
        for token in (b'none', b'F' * 32, b'a' * 31, b'a' * 33):
            self.assertEqual(self.probe(token=token)[:2], (1, []))
        variants = [tree(hci_size='0x3000'), tree(clock_id=12), tree(parent_id=43), tree(parent_provider=1),
                    tree(status='status="disabled";'),
                    tree(ranges=''), tree(ranges='ranges=<0 0 0 0x1000 0 0x100000>;'),
                    tree(phy_ref=2), tree(phy_version=0), tree(phy_version=2), tree(names=['other'] * 14), b'invalid']
        for data in variants:
            with self.subTest(data=data[:24]):
                self.assertEqual(self.probe(data=data)[:2], (1, []))

    def test_gated_and_inaccessible_status_never_reach_hci(self):
        for gates in ((1, 0), (0, 8), (0, 16), (0xffffffff, 0), (0, 0xffffffff)):
            result, addresses, _ = self.probe(gate0=gates[0], gate1=gates[1])
            self.assertIn(result, (2, 3))
            self.assertEqual(addresses, [0x168a0104, 0x168a0004])

    def test_legacy_snapshot_only_reads_exact_standard_and_pinned_vendor_registers(self):
        result, addresses, fields = self.probe(cap=0x0100001f, version=0x300, gate1=7)
        self.assertEqual(result, 4)
        self.assertEqual(addresses, [0x168a0104, 0x168a0004] + [0x16810000 + at for at in
                         (0, 8, 0x34, 0x30, 0x20, 0x24, 0x18, 0x50, 0x54, 0x58, 0x60, 0x70, 0x74, 0x78, 0x80,
                          0x2240, 0x144, 0x140, 0x160, 0x14c)])
        self.assertEqual(fields[2:4], [0x0100001f, 0x300])
        self.assertEqual(fields[21:26], [0x16812240, 0x16810144, 0x16810140, 0x16810160, 0x1681014c])

    def test_mcq_registers_require_both_capability_and_version(self):
        for cap, version, mcq in ((0x4000001f, 0x400, True), (0x4000001f, 0x300, False), (0x1f, 0x400, False)):
            result, addresses, _ = self.probe(cap=cap, version=version)
            self.assertEqual(result, 4)
            self.assertEqual(0x16810300 in addresses, mcq)
            self.assertEqual(0x16810380 in addresses, mcq)

    def test_invalid_hci_stops_before_queue_reads(self):
        for cap, version in ((0xffffffff, 0x400), (31, 0), (31, 0xffffffff)):
            result, addresses, _ = self.probe(cap=cap, version=version)
            self.assertEqual(result, 3)
            self.assertEqual(len(addresses), 4)

    def test_iommu_property_is_observed_without_assuming_direct_dma(self):
        result, _, fields = self.probe(data=tree(iommu='iommus=<2 7>;'))
        self.assertEqual(result, 4)
        self.assertEqual(fields[20], 8)


if __name__ == '__main__':
    unittest.main()
