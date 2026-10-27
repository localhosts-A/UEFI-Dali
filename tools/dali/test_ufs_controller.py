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
  uint64_t sip_trace[8]; uint32_t device_low,device_high,low_held,release_waited,flow_errors,error_reads;
  uint32_t stop_seen,stop_error_reads;
  uint32_t link_polls,event_acks,link_commands,late_injected,link_complete,link_started_at;
  uint32_t early_event_acks,layer_reads_without_uccs,event_ack_value,after_ack_polls,late_completed_at;
} Port;
static uint32_t read32(size_t a, void *ctx) {
  Port *p=ctx; ++p->reads;
  if (a==HCI+0x20 && p->scenario==29 && p->uics==5 && p->hci[0x20/4]==0 && !p->late_injected) {
    p->late_injected=1; p->hci[0x20/4]=4; p->hci[0x38/4]=0x80000010; return 0;
  }
  if (a==HCI+0x20 && p->link_commands==1 && p->uics==6) {
    ++p->link_polls;
    if (p->scenario==45 && !p->late_injected && p->link_polls>=50000) {
      p->late_injected=1; p->hci[0x20/4]=4; p->hci[0x38/4]=0x80000010;
    }
    if (p->scenario==50 && p->link_polls==3) p->hci[0x20/4]=0;
    if (!p->link_complete && (((p->scenario==25 || p->scenario==31 || p->scenario==32 ||
        p->scenario==49 || p->scenario==50 || p->scenario==59) && p->link_polls>=4) ||
        (p->scenario==47 && p->stalled-p->link_started_at>=500000))) {
      p->link_complete=1;
      p->hci[0x20/4]|=0x400; p->hci[0x30/4]=p->scenario==32 ? 8 : p->scenario==49 ? 15 : 9;
      p->hci[0x98/4]=p->scenario==31 ? 10 : 0;
      if (p->scenario==59) p->hci[0x40/4]=0x80000001;
    }
    if (p->scenario==33 && p->link_polls>=4) p->hci[0x20/4]=0x10004;
    if (p->event_acks) {
      ++p->after_ack_polls;
      if (p->scenario==26 || (p->scenario==46 && p->after_ack_polls>=3)) {
        p->hci[0x20/4]=4; p->hci[0x38/4]=0x80000010;
      }
    }
  }
  if (a==AO+0x104 || a==AO+4) return 0;
  if (a==AO+0x148) return p->scenario==16 ? 0 : p->reset_assert;
  if (a==AO+0x14c) return p->scenario==16 ? 0 : p->reset_deassert;
  if (a>=HCI+0x38 && a<=HCI+0x48) {
    if (p->link_commands==1 && p->uics==6 && !(p->hci[0x20/4]&0x400)) ++p->layer_reads_without_uccs;
    ++p->error_reads; uint32_t value=p->hci[(a-HCI)/4]; p->hci[(a-HCI)/4]=0; return value;
  }
  // Inject after the final UCCS poll, before the following ACK. Neither the
  // plain nor the isolated-LINERESET completion may hide this unrelated bit.
  if (a==HCI+0x9c && p->uics==6 && (p->scenario==60 || p->scenario==61)) {
    p->hci[0x20/4]|=8; p->late_injected=1;
  }
  return a>=HCI && a<HCI+0x2a00 ? p->hci[(a-HCI)/4] : UINT32_MAX;
}
static bool write32(size_t a, uint32_t v, void *ctx) {
  Port *p=ctx; ++p->writes;
  if (p->used+2<=1024) { p->trace[p->used++]=a; p->trace[p->used++]=v; }
  if (a==AO+0x148) { p->reset_assert=p->scenario==16 ? p->reset_assert|v : v; return true; }
  if (a==AO+0x14c) { p->reset_deassert=p->scenario==16 ? p->reset_deassert|v : v; return true; }
  if (a<HCI || a>=HCI+0x2a00) return false;
  unsigned o=(a-HCI)/4;
  if (a==HCI+0x24 && p->uics && !p->stop_seen) {
    p->stop_seen=1; p->stop_error_reads=p->error_reads;
    // Time can still advance while handling a timeout. Deliver completion
    // during stop, 10us after the last allowed poll and the failure snapshot.
    if (p->scenario==48 && p->link_commands==1 && !p->link_complete) {
      p->stalled+=10; p->late_completed_at=p->stalled-p->link_started_at;
      p->link_complete=1; p->late_injected=1;
      p->hci[0x20/4]|=0x400; p->hci[0x30/4]=9; p->hci[0x98/4]=0;
    }
  }
  if (p->scenario==8 && p->writes==8) return false;
  if (a==HCI+0x20) {
    if (v&4) {
      ++p->event_acks; p->event_ack_value=v;
      if (!(p->hci[o]&0x400)) ++p->early_event_acks;
      if (p->scenario==40) return false;
      if (p->scenario==39) return true;
    }
    if (p->scenario!=14) p->hci[o]&=~v;
    return true;
  }
  if (a==HCI+0x144) { p->hci[o]=p->scenario==7 ? 1 : 3; return true; }
  if (a==HCI+0x34) {
    if (v==1 && !p->release_waited) ++p->flow_errors;
    if (p->scenario==19 && v==0 && p->device_low && !p->device_high) return false;
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
    if (v==0x16) { ++p->link_commands; p->link_started_at=p->stalled; }
    if (v==0x16 && (p->scenario==20 || p->scenario==21 ||
        (p->scenario>=23 && p->scenario<=28) || (p->scenario>=31 && p->scenario<=41) ||
        (p->scenario>=43 && p->scenario<=56) || p->scenario==59 || p->scenario==61)) {
      p->hci[0x20/4]=4; p->hci[0x30/4]=8; p->hci[0x38/4]=0x80000010;
      if (p->scenario==21) for (unsigned i=1;i<5;i++) p->hci[(0x38+i*4)/4]=i==1 ? 0x80002000 : 0x80000000+i;
      if (p->scenario==23) p->hci[0x38/4]=0x80000011;
      if (p->scenario==24) { p->hci[0x20/4]=0x404; p->hci[0x30/4]=9; }
      if (p->scenario==27) p->hci[0x20/4]=0x804;
      if (p->scenario==28) p->hci[0x40/4]=0x80000001;
      if (p->scenario==34) p->hci[0x38/4]=0;
      if (p->scenario==35) p->hci[0x38/4]=0x10;
      if (p->scenario==36) p->hci[0x38/4]=0x80000001;
      if (p->scenario==37) p->hci[0x38/4]=0x80010010;
      if (p->scenario==38) p->hci[0x20/4]=0x1004;
      if (p->scenario==41) p->hci[0x38/4]=UINT32_MAX;
      if (p->scenario==43) p->hci[0x44/4]=0x80000001;
      if (p->scenario==44) p->hci[0x48/4]=0x80000001;
      if (p->scenario==45) { p->hci[0x20/4]=0; p->hci[0x38/4]=0; }
      if (p->scenario==46) p->hci[0x30/4]=9;
      if (p->scenario==56) p->hci[0x3c/4]=0x80002000;
      // Classification negatives carry a real completion; pending IS4 alone
      // is tested separately and must reach the unchanged deadline unacked.
      if (p->scenario==21 || p->scenario==23 || p->scenario==26 || p->scenario==28 ||
          (p->scenario>=34 && p->scenario<=37) || (p->scenario>=39 && p->scenario<=44) ||
          p->scenario==46 || (p->scenario>=51 && p->scenario<=55) || p->scenario==61) {
        p->hci[0x20/4]=0x404; p->link_complete=1;
      }
      if (p->scenario==51) p->hci[0x34/4]=0;
      if (p->scenario==52) p->hci[0x30/4]=0x38;
      if (p->scenario==53) p->hci[0x30/4]=UINT32_MAX;
      if (p->scenario==54) p->hci[0x98/4]=UINT32_MAX;
      if (p->scenario==55) p->hci[0x9c/4]=UINT32_MAX;
    }
    if ((p->scenario==30 && p->uics==1) || (p->scenario==42 && p->uics==7)) {
      p->hci[0x20/4]=4; p->hci[0x38/4]=0x80000010;
    }
  }
  return true;
}
static void fence(void *ctx) { ++((Port*)ctx)->fences; }
static void stall(uint32_t us, void *ctx) {
  Port *p=ctx; p->stalled+=us;
  if (us==100) p->reset_held=(p->reset_assert&7)==7;
  if (us>=10 && p->device_low && !p->device_high && p->hci[0x34/4]==0) p->low_held=1;
  if (us>=10000 && p->device_high) p->release_waited=1;
}
static uint64_t sip(uint32_t svc,uint32_t sel,uint32_t a,uint32_t b,void *ctx) {
  Port *p=ctx;
  if (p->sips<8) p->sip_trace[p->sips]=((uint64_t)sel<<32)|((uint64_t)a<<16)|b;
  ++p->sips;
  if (svc!=0xc2000276 || (sel!=512 && sel!=8 && sel!=2) || a>1 || b>1) return UINT64_MAX;
  if (sel==2) {
    if (a==0) ++p->device_low;
    else { ++p->device_high; if (!p->low_held) ++p->flow_errors; }
    if ((p->scenario==17 && a==0) || (p->scenario==18 && a==1) || p->scenario==22) return UINT64_MAX;
  }
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
  out[32]=c.DeviceResetAsserted; out[33]=c.DeviceResetReleaseAttempted; out[34]=c.DeviceResetReleased;
  out[35]=c.DeviceResetCleanupAttempted; out[36]=c.DeviceResetCleanupStatus;
  out[37]=p.device_low; out[38]=p.device_high; out[39]=p.low_held; out[40]=p.release_waited; out[41]=p.flow_errors;
  out[42]=c.UicFailure.Captured; out[43]=c.UicFailure.InterruptStatus; out[44]=c.UicFailure.HostStatus; out[45]=c.UicFailure.HostEnable;
  for (unsigned i=0;i<3;i++) out[46+i]=c.UicFailure.Arguments[i];
  for (unsigned i=0;i<5;i++) out[49+i]=c.UicFailure.LayerErrors[i];
  out[54]=p.error_reads; out[55]=c.SipSelector; out[56]=c.SipArg1; out[57]=c.SipArg2;
  out[58]=p.stop_error_reads; out[59]=p.stop_seen;
  for (unsigned i=0;i<4;i++) out[60+i]=p.sip_trace[i];
  out[64]=c.LinkResetEvent.Captured; out[65]=c.LinkResetAccepted; out[66]=c.LinkResetAcked;
  out[67]=c.LinkResetEvent.InterruptStatus; out[68]=c.LinkResetEvent.HostStatus; out[69]=c.LinkResetEvent.HostEnable;
  for (unsigned i=0;i<3;i++) out[70+i]=c.LinkResetEvent.Arguments[i];
  for (unsigned i=0;i<5;i++) out[73+i]=c.LinkResetEvent.LayerErrors[i];
  out[78]=p.event_acks; out[79]=p.link_polls; out[80]=p.link_commands;
  out[81]=p.early_event_acks; out[82]=p.layer_reads_without_uccs; out[83]=p.event_ack_value;
  out[84]=c.LinkResetPending; out[85]=c.LinkResetFirstIs; out[86]=c.LinkResetPolls;
  out[87]=p.link_complete; out[88]=p.late_completed_at; out[89]=p.late_injected;
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
        out, trace = (ctypes.c_uint64 * 96)(), (ctypes.c_uint64 * 1024)()
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
        self.assertEqual(out[3], 4)
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
        self.assertEqual((out[0], out[3]), (7, 6))
        out, _ = self.run_case(7)
        self.assertEqual((out[0], out[2], out[3], out[8]), (3, 0, 4, 2))
        self.assertGreaterEqual(out[20], 3000)
        self.assertEqual((out[9], out[10]), (0x16810144, 1))

    def test_stop_failure_keeps_instance_poisoned_and_never_returns_stopped(self):
        out, _ = self.run_case(6)
        self.assertEqual(out[0], 7)
        self.assertEqual((out[16], out[18]), (3, 3))
        self.assertGreaterEqual(out[20], 200700)
        self.assertEqual((out[42], out[54], out[58], out[59]), (0, 0, 0, 1))

    def test_device_reset_source_pulse_and_settle_precede_hce_enable(self):
        out, trace = self.run_case()
        self.assertEqual(out[32:42], [1, 1, 1, 0, 0, 1, 1, 1, 1, 0])
        self.assertEqual(out[60:64], [512 << 32, (512 << 32) | (1 << 16), 2 << 32, (2 << 32) | (1 << 16)])
        self.assertEqual([v for a, v in trace if a == 0x16810034][:2], [0, 1])
        self.assertGreaterEqual(out[20], 15710)

    def test_reset_failures_release_once_without_replay_or_fault_loss(self):
        for scenario, result, released in ((17, 6, 1), (18, 6, 0), (19, 4, 1), (22, 6, 0)):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[5], out[6], out[8]), (result, 0, 1, 0, 8))
                self.assertEqual(out[37:39], [1, 1])
                self.assertEqual(out[34], released)
                self.assertEqual(out[1:4], out[13:16])
                self.assertEqual(out[42], 0)
        out, _ = self.run_case(17)
        self.assertEqual((out[28], out[35], out[36]), ((1 << 64) - 1, 1, 0))
        out, _ = self.run_case(18)
        self.assertEqual(out[35], 0)  # Failed release is never submitted a second time.
        out, _ = self.run_case(22)
        self.assertEqual(out[36], (1 << 64) - 1)

    def test_first_uic_failure_snapshot_precedes_stop_and_reads_layer_codes_once(self):
        for scenario in (23, 21):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[8], out[9], out[10]), (4, 6, 6, 0x16810020, 0x404))
                self.assertEqual(out[42:46], [1, 0x404, 8, 1])
                self.assertEqual(out[46:49], [0, 0, 0])
                expected = [0x80000011, 0, 0, 0, 0] if scenario == 23 else [0x80000010, 0x80002000, 0x80000002, 0x80000003, 0x80000004]
                self.assertEqual(out[49:54], expected)
                self.assertEqual(out[54], 5)
                self.assertEqual(out[58:60], [5, 1])
                self.assertTrue(out[7])
                self.assertEqual(out[12], 4)

    def test_failure_snapshot_skips_uninitialized_power_and_hce_phases(self):
        for scenario in (1, 2, 7, 17):
            out, _ = self.run_case(scenario)
            self.assertEqual((out[42], out[54]), (0, 0))

    def test_isolated_link_reset_waits_for_real_completion_without_resubmitting(self):
        for scenario in (24, 25):
            with self.subTest(scenario=scenario):
                out, trace = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[42], out[54]), (7, 12, 0, 5))
                self.assertEqual(out[64:67], [1, 1, 1])
                self.assertEqual(out[73:78], [0x80000010, 0, 0, 0, 0])
                self.assertEqual((out[78], out[80]), (1, 1))
                self.assertEqual(sum(a == 0x16810020 and v == 0x404 for a, v in trace), 1)
                self.assertEqual((out[67], out[81], out[82], out[83]), (0x404, 0, 0, 0x404))
                self.assertFalse(any(a == 0x16810020 and v == 4 for a, v in trace))
                if scenario == 25:
                    self.assertGreaterEqual(out[79], 4)
                    self.assertEqual((out[68], out[85]), (9, 4))

    def test_actual_b67_latched_status_reaches_completion_before_any_layer_read_or_ack(self):
        out, trace = self.run_case(49)
        self.assertEqual((out[0], out[2], out[54], out[80]), (7, 12, 5, 1))
        self.assertEqual(out[64:70], [1, 1, 1, 0x404, 15, 1])
        self.assertEqual(out[81:86], [0, 0, 0x404, 1, 4])
        self.assertGreaterEqual(out[86], 4)
        self.assertEqual(sum(a == 0x16810020 and v & 4 != 0 for a, v in trace), 1)

    def test_link_reset_never_substitutes_for_uccs_or_extends_deadline(self):
        for scenario in (20, 45, 56):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[8], out[80]), (3, 6, 6, 1))
                self.assertEqual(out[64:67], [0, 0, 0])
                self.assertEqual(out[49], 0x80000010)
                self.assertEqual((out[42], out[54], out[78], out[81]), (1, 5, 0, 0))
                self.assertEqual(out[84], 1)
                self.assertEqual(out[20], 515710)
                self.assertEqual(out[1:4], out[13:16])

    def test_link_reset_rejects_other_layers_lanes_unknown_and_invalid_codes(self):
        for scenario in (21, 23, 28, 34, 35, 36, 37, 41, 43, 44):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[42], out[54]), (4, 6, 1, 5))
                self.assertEqual(out[64:67], [1, 0, 0])
                self.assertEqual(out[49:54], out[73:78])  # First read-clear codes retained.
                self.assertEqual((out[78], out[80], out[58]), (0, 1, 5))

    def test_link_reset_completion_at_deadline_passes_but_later_completion_cannot(self):
        for scenario, result, commands in ((47, 7, 12), (48, 3, 6)):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[80]), (result, commands, 1))
                self.assertEqual(out[64:67], [1, 1, 1] if result == 7 else [0, 0, 0])
                self.assertEqual(out[20], 515710 if scenario == 47 else 515720)
                self.assertEqual(out[78], int(result == 7))
                if scenario == 48:
                    self.assertEqual(out[87:90], [1, 500010, 1])
                    self.assertEqual(out[42:44], [1, 4])
                    self.assertEqual(out[1:4], out[13:16])
                    self.assertEqual((out[5], out[6], out[7]), (1, 0, 1))

    def test_link_completion_ack_rejects_unrelated_status_arriving_after_final_poll(self):
        for scenario in (60, 61):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[5], out[6], out[7], out[80]), (4, 6, 1, 0, 1, 1))
                self.assertEqual((out[9], out[10], out[89]), (0x16810020, 8, 1))
                self.assertEqual(out[66], 0)
                self.assertEqual(out[1:4], out[13:16])

    def test_link_reset_does_not_swallow_fatal_reserved_preexisting_or_other_commands(self):
        for scenario, commands in ((27, 6), (38, 6), (29, 5), (30, 1), (42, 7)):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[42], out[54]), (4, commands, 1, 5))
                self.assertEqual(out[64:67], [0, 0, 0])
                self.assertEqual(out[78], 0)

    def test_repeated_sticky_fatal_or_failed_event_ack_stops_without_replay(self):
        for scenario in (26, 39, 40, 46):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[42], out[54]), (4, 6, 1, 10))
                self.assertEqual(out[64:67], [1, 1, int(scenario == 46)])
                self.assertEqual((out[78], out[80]), (1, 1))
                self.assertEqual(out[73], 0x80000010)
                self.assertEqual(out[1:4], out[13:16])

    def test_fatal_during_deferred_wait_stops_without_ack_or_deadline_extension(self):
        out, _ = self.run_case(33)
        self.assertEqual((out[0], out[2], out[42], out[54], out[78], out[80]), (4, 6, 1, 5, 0, 1))
        self.assertEqual(out[64:67], [0, 0, 0])
        self.assertEqual(out[84], 1)
        self.assertLess(out[20], 16000)

    def test_deferred_completion_cannot_hide_lost_is_bad_hcs_hce_or_arguments(self):
        for scenario in (50, 51, 52, 53, 54, 55, 59):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[42], out[66], out[78], out[80]), (4, 6, 1, 0, 0, 1))
                self.assertEqual(out[1:4], out[13:16])

    def test_link_reset_cannot_hide_failed_result_or_absent_device(self):
        for scenario, result in ((31, 5), (32, 3)):
            with self.subTest(scenario=scenario):
                out, _ = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[8], out[80]), (result, 6, 6, 1))
                self.assertEqual(out[64:67], [1, 1, int(scenario == 32)])
                self.assertEqual(out[78], int(scenario == 32))
                if scenario == 31:
                    self.assertEqual(out[11], 10)
