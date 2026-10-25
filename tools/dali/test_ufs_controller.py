"""Compile the production control plane; emulate only MMIO/SiP/timing."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_ufs_probe import TOKEN, tree


class UfsControllerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler or not shutil.which('fdtput'):
            raise unittest.SkipTest('C compiler, dtc/fdtput and libfdt required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        root = Path(__file__).resolve().parents[2]
        cls.base = base
        (base / 'Library').mkdir()
        (base / 'Base.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t UINT32; typedef int32_t INT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef char CHAR8; typedef bool BOOLEAN;
#define VOID void
#define STATIC static
#define CONST const
#define TRUE true
#define FALSE false
''')
        (base / 'Library/BaseMemoryLib.h').write_text('''#include <Base.h>
#include <string.h>
#define ZeroMem(p,n) memset(p,0,n)
#define CompareMem memcmp
''')
        (base / 'Library/FdtLib.h').write_text('''#include <libfdt.h>
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
''')
        (base / 'harness.c').write_text('''#include <Library/Mt6991UfsController.h>
#include <string.h>
#define HCI 0x16810000UL
#define AO 0x168a0000UL
typedef struct {
  uint32_t hci[0x2a00/4], dme[65536], scenario, writes, reads, uics, sips, fences, stalled;
  uint64_t trace[1024]; size_t used; uint32_t reset_assert,reset_deassert,reset_held;
} Port;
static uint32_t read32(size_t a, void *ctx) {
  Port *p=ctx; ++p->reads;
  if (a==AO+0x104 || a==AO+4) return 0;
  if (a==AO+0x148) return p->scenario==16 ? 0 : p->reset_assert;
  if (a==AO+0x14c) return p->scenario==16 ? 0 : p->reset_deassert;
  return a>=HCI && a<HCI+0x2a00 ? p->hci[(a-HCI)/4] : UINT32_MAX;
}
static bool write32(size_t a, uint32_t v, void *ctx) {
  Port *p=ctx; ++p->writes;
  if (p->used+2<=1024) { p->trace[p->used++]=a; p->trace[p->used++]=v; }
  if (a==AO+0x148) { p->reset_assert=p->scenario==16 ? p->reset_assert|v : v; return true; }
  if (a==AO+0x14c) { p->reset_deassert=p->scenario==16 ? p->reset_deassert|v : v; return true; }
  if (a<HCI || a>=HCI+0x2a00) return false;
  unsigned o=(a-HCI)/4;
  if (p->scenario==8 && p->writes==8) return false;
  if (a==HCI+0x20) { if (p->scenario!=14) p->hci[o]&=~v; return true; }
  if (a==HCI+0x144) { p->hci[o]=p->scenario==7 ? 1 : 3; return true; }
  if (a==HCI+0x34) {
    if (p->scenario==2 && v==1) v=0;
    if (p->scenario==6 && v==0) return true;
    p->hci[o]=v; p->hci[0x30/4]=v ? 8 : 0;
    if (v && p->scenario==15) p->hci[0x50/4]=0x12340000;
    return true;
  }
  p->hci[o]=v;
  if (a==HCI+0x90) {
    ++p->uics;
    if (p->scenario==3) return true;
    uint32_t attr=p->hci[0x94/4]>>16;
    if (v==1) p->hci[0x9c/4]=p->dme[attr];
    else if (v==2) p->dme[attr]=p->hci[0x9c/4];
    else if (v==0x16 && p->scenario!=10) p->hci[0x30/4]|=1;
    p->hci[0x98/4]=p->scenario==4 ? 10 : 0;
    p->hci[0x20/4]|=p->scenario==5 ? 0x10400 : 0x400;
  }
  return true;
}
static void fence(void *ctx) { ++((Port*)ctx)->fences; }
static void stall(uint32_t us, void *ctx) {
  Port *p=ctx; p->stalled+=us;
  if (us==100) p->reset_held=(p->reset_assert&7)==7;
}
static uint64_t sip(uint32_t svc,uint32_t sel,uint32_t a,uint32_t b,void *ctx) {
  Port *p=ctx; ++p->sips;
  if (svc!=0xc2000276 || (sel!=512 && sel!=8) || a>1 || b>1) return UINT64_MAX;
  return p->scenario==1 ? UINT64_MAX : 0;
}
int run(const void *fdt, int enabled, const char *token, unsigned scenario,
        uint64_t *out, uint64_t *trace) {
  Port p={.scenario=scenario}; MT6991_UFS_CONTROLLER c={0};
  p.reset_assert=0x1000; p.reset_deassert=0x2000;
  p.hci[0]=0x55c7051f; p.hci[8/4]=0x400; p.hci[0x2240/4]=0x10470000;
  p.hci[0x144/4]=scenario==7 || scenario==9 ? 0 : 3;
  p.hci[0x140/4]=0x102; p.hci[0x160/4]=0x10553070;
  p.hci[4/4]=0x080b0207; p.hci[0x380/4]=0x1f00;
  if (scenario==11) p.hci[0x58/4]=1;
  if (scenario==12) p.hci[0x300/4]=1;
  if (scenario==13) p.hci[0x2240/4]=0x10460000;
  if (scenario==14) p.hci[0x20/4]=0x400;
  p.dme[0x15a9]=0x180; p.dme[0xd0a6]=0x40; p.dme[0xd0a1]=0x20;
  MT6991_UFS_IO io={read32,write32,fence,stall,sip,&p};
  out[0]=Mt6991UfsStart(fdt,enabled,token,&io,&c);
  out[1]=p.writes; out[2]=p.uics; out[3]=p.sips; out[4]=p.reads;
  out[5]=c.Failed; out[6]=c.Ready; out[7]=c.Stopped; out[8]=c.FaultStage;
  out[9]=c.FaultAddress; out[10]=c.FaultValue; out[11]=c.UicResult;
  out[12]=Mt6991UfsStart(fdt,enabled,token,&io,&c);
  out[13]=p.writes; out[14]=p.uics; out[15]=p.sips;
  out[16]=Mt6991UfsStop(&c); out[17]=p.writes;
  out[18]=Mt6991UfsStop(&c); out[19]=p.writes;
  out[20]=p.stalled; out[21]=p.fences; out[22]=p.used;
  out[23]=p.dme[0xd0a6]; out[24]=p.dme[0xd0a1]; out[25]=p.dme[0xd09e];
  out[26]=p.hci[0x140/4]; out[27]=p.hci[0x160/4]; out[28]=c.SipStatus;
  out[29]=p.reset_assert; out[30]=p.reset_deassert; out[31]=p.reset_held;
  memcpy(trace,p.trace,sizeof p.trace); return 0;
}
''')
        pkg = root / 'Silicon/MediaTek/MT6991Pkg'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(pkg / 'Include'),
                        str(pkg / 'Library/PlatformLib/UfsController.c'), str(base / 'harness.c'),
                        '-lfdt', '-o', str(base / 'controller.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'controller.so'))
        cls.lib.run.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint,
                                ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def bindings(self, override=None):
        path = self.base / 'fdt.dtb'
        path.write_bytes(tree())
        reset = '/syscon@168a0000/reset-controller'
        subprocess.run(['fdtput', '-c', str(path), reset], check=True)
        properties = {
            ('/soc', 'dma-ranges'): ('x', [0, 0, 0, 0, 16, 0]),
            (reset, 'compatible'): ('s', ['ti,syscon-reset']),
            (reset, '#reset-cells'): ('x', [1]), (reset, 'phandle'): ('x', [4]),
            (reset, 'ti,reset-bits'): ('x', [0x48, 8, 0x4c, 8, 0, 0, 0x1c] +
                                       sum(([0x148, i, 0x14c, i, 0, 0, 0x1c] for i in range(3)), [])),
            ('/soc/ufshci@16810000', 'resets'): ('x', [4, 1, 4, 2, 4, 3]),
            ('/soc/ufshci@16810000', 'reset-names'): ('s', ['unipro_rst', 'crypto_rst', 'hci_rst']),
            ('/soc/ufshci@16810000', 'mediatek,ufs-rtff-mtcmos'): ('x', []),
        }
        if override:
            properties.update(override)
        for (node, prop), (kind, values) in properties.items():
            values = [hex(v)[2:] if kind == 'x' else v for v in values]
            subprocess.run(['fdtput', '-t', kind, str(path), node, prop, *values], check=True)
        return path.read_bytes()

    def run_case(self, scenario=0, data=None, enabled=True, token=TOKEN):
        data = self.bindings() if data is None else data
        out, trace = (ctypes.c_uint64 * 32)(), (ctypes.c_uint64 * 1024)()
        self.lib.run(ctypes.create_string_buffer(data), enabled, token, scenario, out, trace)
        return list(out), list(zip(trace[:out[22]:2], trace[1:out[22]:2]))

    def test_default_off_and_rejected_bindings_never_touch_hardware(self):
        out, _ = self.run_case(data=b'invalid', enabled=False, token=b'none')
        self.assertEqual((out[0], out[1], out[3], out[4]), (0, 0, 0, 0))
        variants = [b'invalid', tree(), self.bindings({('/soc', 'dma-ranges'): ('x', [0, 0, 0, 1, 16, 0])}),
                    self.bindings({('/soc/ufshci@16810000', 'resets'): ('x', [4, 0, 4, 2, 4, 3])}),
                    self.bindings({('/soc', 'dma-coherent'): ('x', [])}),
                    self.bindings({('/soc/ufshci@16810000', 'mediatek,ufs-support-va09'): ('x', [])})]
        for data in variants:
            out, _ = self.run_case(data=data)
            self.assertEqual((out[0], out[1], out[3], out[4]), (1, 0, 0, 0))
        out, _ = self.run_case(token=b'bad')
        self.assertEqual((out[0], out[1], out[3], out[4]), (1, 0, 0, 0))

    def test_active_legacy_or_mcq_and_wrong_ip_rejected_before_writes(self):
        for scenario, expected in ((11, 2), (12, 2), (13, 1)):
            out, _ = self.run_case(scenario)
            self.assertEqual((out[0], out[1], out[3]), (expected, 0, 0))

    def test_actual_production_sequence_reaches_link_without_dma_or_media(self):
        out, trace = self.run_case()
        self.assertEqual((out[0], out[5], out[6], out[7]), (7, 0, 1, 0))
        self.assertEqual(out[2], 12)
        self.assertEqual(out[3], 2)
        self.assertEqual(out[23:28], [0xd, 0, 0x100, 0x182, 0x02453070])
        self.assertEqual([v for a, v in trace if a == 0x168a0148], [0x1004, 0x1006, 0x1007])
        self.assertEqual([v for a, v in trace if a == 0x168a014c], [0x2001, 0x2003, 0x2007])
        self.assertEqual(out[29:32], [0x1007, 0x2007, 1])
        allowed = {0x18, 0x20, 0x24, 0x34, 0x60, 0x80, 0x90, 0x94, 0x98, 0x9c, 0x140, 0x144, 0x160}
        self.assertTrue(all(a in (0x168a0148, 0x168a014c) or a - 0x16810000 in allowed for a, _ in trace))
        self.assertTrue(all(v == 0 for a, v in trace if a - 0x16810000 in (0x24, 0x60, 0x80)))
        self.assertEqual(out[21], len(trace))
        self.assertGreaterEqual(out[20], 700)

    def test_repeated_start_and_stop_do_not_repeat_commands(self):
        out, _ = self.run_case()
        self.assertEqual(out[12:16], [7, out[1], out[2], out[3]])
        self.assertEqual(out[16], 8)
        self.assertEqual(out[18], 8)
        self.assertEqual(out[17], out[19])

    def test_reset_masked_updates_also_support_zero_readback_set_registers(self):
        out, trace = self.run_case(16)
        self.assertEqual(out[0], 7)
        self.assertEqual([v for a, v in trace if a == 0x168a0148], [4, 2, 1])
        self.assertEqual([v for a, v in trace if a == 0x168a014c], [1, 2, 4])
        self.assertEqual(out[29:32], [0x1007, 0x2007, 1])

    def test_hce_and_uic_timeouts_are_bounded_poisoned_and_stopped(self):
        for scenario, commands in ((2, 0), (3, 1), (10, 6)):
            out, _ = self.run_case(scenario)
            self.assertEqual((out[0], out[2], out[5], out[6], out[7]), (3, commands, 1, 0, 1))
            self.assertEqual(out[1], out[13])
            self.assertEqual(out[2], out[14])
            self.assertLessEqual(out[20], 600700)

    def test_uic_result_and_fatal_error_are_not_reported_as_link_success(self):
        for scenario, result in ((4, 5), (5, 4)):
            out, _ = self.run_case(scenario)
            self.assertEqual((out[0], out[2], out[5], out[6], out[7]), (result, 1, 1, 0, 1))
        out, _ = self.run_case(4)
        self.assertEqual(out[11], 10)

    def test_stale_completion_or_reactivated_old_queue_prevents_submission(self):
        for scenario, expected in ((14, 4), (15, 2)):
            out, _ = self.run_case(scenario)
            self.assertEqual((out[0], out[2], out[5], out[6], out[7]), (expected, 0, 1, 0, 1))

    def test_secure_monitor_or_mmio_failure_poison_without_retry(self):
        for scenario, expected in ((1, 6), (8, 4)):
            out, _ = self.run_case(scenario)
            self.assertEqual((out[0], out[5], out[6], out[2]), (expected, 1, 0, 0))
            self.assertEqual(out[1:4], out[13:16])

    def test_reference_clock_ack_and_timeout_notification(self):
        out, _ = self.run_case(9)
        self.assertEqual((out[0], out[3]), (7, 4))
        out, _ = self.run_case(7)
        self.assertEqual((out[0], out[2], out[3], out[8]), (3, 0, 4, 2))
        self.assertGreaterEqual(out[20], 3000)
        self.assertEqual((out[9], out[10]), (0x16810144, 1))

    def test_stop_failure_keeps_instance_poisoned_and_never_returns_stopped(self):
        out, _ = self.run_case(6)
        self.assertEqual(out[0], 7)
        self.assertEqual((out[16], out[18]), (3, 3))
        self.assertGreaterEqual(out[20], 200700)
