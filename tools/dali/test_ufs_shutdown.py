"""Execute production shutdown + wire policy + DMA with hardware fault injection."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsShutdownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base/'Library').mkdir()
        (base/'Uefi.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32;
typedef uint64_t UINT64; typedef int32_t INT32; typedef size_t UINTN;
typedef char CHAR8; typedef bool BOOLEAN; typedef uint64_t EFI_STATUS;
#define VOID void
#define CONST const
#define STATIC static
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_BAD_BUFFER_SIZE 4
#define EFI_NOT_READY 6
#define EFI_DEVICE_ERROR 7
#define EFI_WRITE_PROTECTED 8
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NOT_FOUND 14
#define EFI_ACCESS_DENIED 15
#define EFI_TIMEOUT 18
#define EFI_ALREADY_STARTED 20
#define EFI_ERROR(s) ((s)!=0)
#define MAX_UINTN SIZE_MAX
#define MAX_UINT16 UINT16_MAX
#define MAX_UINT32 UINT32_MAX
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_SIZE 4096
#define EFI_SIZE_TO_PAGES(n) (((n)+4095UL)/4096UL)
''')
        (base/'Base.h').write_text('#include <Uefi.h>\n')
        (base/'Library/BaseMemoryLib.h').write_text('#include <string.h>\n#define ZeroMem(p,n) memset(p,0,n)\n#define CopyMem memcpy\n#define CompareMem memcmp\n')
        (base/'harness.c').write_text('''#include <stdlib.h>
#include <string.h>
#include "UfsShutdown.h"
#define HCI 0x16810000UL
typedef struct {
 MT6991_UFS_DMA d; MT6991_UFS_PENDING p; MT6991_UFS_GEOMETRY g[8];
 volatile uint64_t counts[8]; DALI_UFS_SHUTDOWN state;
 uint32_t reg[0x100/4],scenario,writes,reads,commands,polls,stalled,allocs,frees,live,fences,bad;
 uint64_t next_device,old_trl; uint8_t user[512];
} Port;
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static void fence(void *ctx) { ++((Port*)ctx)->fences; }
static EFI_STATUS allocate(MT6991_DMA_REGION *r,size_t pages,uint64_t limit,void *ctx) {
 Port *p=ctx; ++p->allocs;
 if(p->scenario==14)return EFI_OUT_OF_RESOURCES;
 void *cpu=aligned_alloc(4096,pages*4096); if(!cpu)return EFI_OUT_OF_RESOURCES;
 if(p->next_device+pages*4096-1>limit) {free(cpu);return EFI_OUT_OF_RESOURCES;}
 r->Cpu=cpu;r->Device=p->next_device;r->Pages=pages;r->Live=true;
 p->next_device+=0x10000;++p->live;return 0;
}
static EFI_STATUS release(MT6991_DMA_REGION *r,void *ctx) {
 Port *p=ctx;++p->frees;
 if(p->scenario==18&&p->frees==1)return EFI_DEVICE_ERROR;
 free(r->Cpu);r->Live=false;--p->live;return 0;
}
static void complete(Port *p) {
 MT6991_UFS_PENDING *q=&p->p; uint8_t *r=q->Response;
 put32(q->Trd+8,0);memset(r,0,q->ResponseBytes);
 r[0]=q->Transaction|0x20;r[2]=q->Lun;r[3]=q->Tag;
 if(q->Transaction==0x16) {r[5]=q->Request[5];memcpy(r+12,q->Request+12,4);}
 if(q->Data)memset(q->Data,0x5a,q->Expected);
 if(p->scenario==2)put32(q->Trd+8,1);
 if(p->scenario==3||(p->scenario==11&&q->Lun==2))r[7]=2;
 if(p->scenario==4)r[3]^=1;
 if(p->scenario==16)r[2]^=1;
 if(p->scenario==17)r[6]=1;
 p->reg[0x58/4]=0;p->reg[0x20/4]=p->scenario==5?0x10001:1;
}
static uint32_t read32(size_t a,void *ctx) {
 Port *p=ctx;++p->reads;
 if(a<HCI||a>=HCI+0x100)return UINT32_MAX;
 unsigned o=(a-HCI)/4;
 if(o==0x58/4&&p->reg[o]&&p->d.Busy&&++p->polls>=4&&p->scenario!=1&&p->scenario!=22)complete(p);
 return p->reg[o];
}
static bool write32(size_t a,uint32_t v,void *ctx) {
 Port *p=ctx;++p->writes;
 if(a<HCI||a>=HCI+0x100)return false;
 unsigned off=(unsigned)(a-HCI);
 if(off!=0x20&&off!=0x50&&off!=0x54&&off!=0x58&&off!=0x60&&off!=0x80) {++p->bad;return false;}
 if(p->scenario==10&&off==0x50)return false;
 if(off==0x20) {p->reg[off/4]&=~v;return true;}
 if(p->scenario==9&&off==0x60&&v==0)return true;
 if(off==0x58) {
  ++p->commands;p->polls=0;
  const uint8_t cdb[10]={0x35};
  if(v!=1||!p->d.Busy||p->p.Transaction!=1||p->p.Opcode!=0x35||p->p.Expected||memcmp(p->p.Request+16,cdb,sizeof cdb))++p->bad;
 }
 p->reg[off/4]=v;return !(p->scenario==23&&off==0x58);
}
static void stall(uint32_t us,void *ctx) {((Port*)ctx)->stalled+=us;}
static void inflight(Port *p,bool query) {
 void *page;uint64_t dev,id,datadev;
 if(Mt6991UfsDmaAllocate(&p->d,1,&page)||Mt6991UfsDmaMap(&p->d,Mt6991DmaCommon,page,4096,&dev,&id))abort();
 uint8_t *t=page,*q=(uint8_t*)page+1024;
 put32(t,query?0x14000000:0x15000000);put32(t+8,15);put32(t+16,(uint32_t)(dev+1024));
 t[24]=query?8:14;t[26]=8;
 q[0]=query?0x16:1;q[1]=query?0:0x41;q[2]=2;q[3]=200;
 if(query) {q[5]=1;q[12]=5;}
 else {
  p->g[2].BlockSize=512; q[14]=2;q[16]=0x28;q[24]=1;t[28]=1;t[30]=22;
  if(Mt6991UfsDmaMap(&p->d,Mt6991DmaDeviceWrite,p->user,512,&datadev,&id))abort();
  put32(q+88,(uint32_t)datadev);put32(q+100,511);
 }
 if(Mt6991UfsValidateTransfer(&p->d,dev,p->g,NULL,false,&p->p)||Mt6991UfsDmaSubmit(&p->d))abort();
 p->reg[0x50/4]=(uint32_t)dev;p->reg[0x58/4]=1;p->old_trl=dev;
}
void run(unsigned scenario,uint64_t *out) {
 Port p={0};p.scenario=scenario;p.next_device=0x10000000;
 p.reg[0x34/4]=1;p.reg[0x60/4]=1;p.reg[0x80/4]=1;
 p.g[0]=(MT6991_UFS_GEOMETRY){1024,4096,true,false,false};p.g[2]=(MT6991_UFS_GEOMETRY){64577536,4096,true,false,false};
 MT6991_DMA_OPS ops={allocate,release,fence,&p};
 MT6991_UFS_IO io={read32,write32,fence,stall,NULL,&p};
 if(scenario!=20&&Mt6991UfsDmaInit(&p.d,&ops,UINT32_MAX))abort();
 memset(p.user,0x33,sizeof p.user);
 if(scenario==13||scenario==18||scenario==17||scenario==1)inflight(&p,scenario==17);
 if(scenario==6)p.reg[0x34/4]=0;
 if(scenario==7)p.reg[0x78/4]=1;
 if(scenario==8)p.reg[0x58/4]=1;
 if(scenario==12)p.counts[0]=UINT64_MAX;
 if(scenario==15)p.d.NextId=UINT64_MAX;
 if(scenario==21)memset(p.g,0,sizeof p.g);
 if(scenario==24)p.g[0].BlockSize=1536;
 out[0]=DaliUfsDrainAndSync(&p.d,&p.p,p.g,p.counts,&io,&p.state);
 out[1]=p.state.Attempted;out[2]=p.state.Completed;out[3]=p.state.Commands;out[4]=p.state.SyncedLuns;
 out[5]=p.d.Poisoned;out[6]=p.d.Busy;out[7]=p.writes;out[8]=p.commands;out[9]=p.stalled;
 out[10]=p.allocs;out[11]=p.frees;out[12]=p.counts[0];out[13]=p.counts[2];out[18]=p.live;
 out[24]=p.user[0];out[25]=p.bad;out[27]=p.reads;out[28]=p.old_trl!=0;
 out[29]=p.old_trl!=0&&p.old_trl!=p.reg[0x50/4];
 out[14]=DaliUfsDrainAndSync(&p.d,&p.p,p.g,p.counts,&io,&p.state);
 out[15]=p.writes;out[16]=p.allocs;out[30]=p.reads;
 out[31]=p.state.Attempted;out[32]=p.state.Completed;out[33]=p.state.Commands;out[34]=p.state.SyncedLuns;out[35]=p.state.Status;
 if(p.d.Initialized) {
  out[17]=Mt6991UfsDmaReleaseAll(&p.d);
  Mt6991UfsDmaPoison(&p.d);out[19]=Mt6991UfsDmaStopped(&p.d,1);
  out[20]=Mt6991UfsDmaReleaseAll(&p.d);out[21]=Mt6991UfsDmaStopped(&p.d,0);
  out[22]=Mt6991UfsDmaReleaseAll(&p.d);out[23]=p.live;
 }
}
''')
        root = Path(__file__).resolve().parents[2]
        pkg = root/'Silicon/MediaTek/MT6991Pkg'
        host = pkg/'Drivers/UfsHostDxe'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(pkg/'Include'), '-I', str(host),
                        str(pkg/'Library/UfsDmaLib/UfsDma.c'), str(host/'UfsPolicy.c'), str(host/'UfsShutdown.c'),
                        str(base/'harness.c'), '-o', str(base/'shutdown.so')], check=True)
        cls.lib = ctypes.CDLL(str(base/'shutdown.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, scenario=0):
        out = (ctypes.c_uint64*40)()
        self.lib.run(scenario, out)
        return list(out)

    def test_private_owned_no_data_syncs_each_learned_lun_without_new_control_commands(self):
        out = self.run_case()
        self.assertEqual(out[:9], [0, 1, 1, 2, 5, 0, 0, out[7], 2])
        self.assertEqual(out[12:14], [1, 1])
        self.assertEqual(out[25], 0)  # Only legacy queue/IS/doorbell writes.
        self.assertEqual(out[11], 0)  # Own descriptor remains retained for HCE0.
        self.assertEqual(out[17:24], [6, 1, 6, 6, 0, 0, 0])

    def test_success_or_failure_is_idempotent_and_does_not_repeat_io(self):
        for scenario in (0, 1, 2, 3, 4, 10, 11, 14, 15, 18, 20, 21):
            with self.subTest(scenario=scenario):
                out = self.run_case(scenario)
                self.assertEqual((out[14], out[15], out[16], out[30]), (out[0], out[7], out[10], out[27]))
                self.assertEqual(out[31:36], [*out[1:5], out[0]])

    def test_inflight_read_finishes_copies_back_and_retires_mapping_before_sync(self):
        out = self.run_case(13)
        self.assertEqual((out[0], out[3], out[4], out[11], out[24], out[28], out[29]), (0, 2, 5, 1, 0x5a, 1, 1))
        self.assertEqual(out[23], 0)

    def test_inflight_timeout_never_replays_or_releases_before_hce_zero(self):
        out = self.run_case(1)
        self.assertEqual((out[0], out[3], out[5], out[6], out[8], out[9]), (18, 0, 1, 1, 0, 3000000))
        self.assertEqual(out[17], 6)
        self.assertEqual(out[20:24], [6, 0, 0, 0])

    def test_sync_timeout_and_uncertain_doorbell_never_resubmit(self):
        for scenario, result in ((22, 18), (23, 7)):
            out = self.run_case(scenario)
            self.assertEqual((out[0], out[3], out[4], out[5], out[6], out[8]), (result, 1, 0, 1, 1, 1))
            self.assertEqual(out[14:17], [result, out[7], out[10]])
            self.assertEqual(out[30], out[27])
            self.assertEqual(out[31:36], [*out[1:5], out[0]])
            self.assertEqual(out[9], 3000000 if scenario == 22 else 0)

    def test_invalid_learned_geometry_never_allocates_or_rings(self):
        out = self.run_case(24)
        self.assertEqual((out[0], out[3], out[4], out[5], out[8], out[10]), (7, 0, 0, 1, 0, 0))

    def test_bad_ocs_target_tag_lun_or_fatal_status_poison_without_second_command(self):
        for scenario in (2, 3, 4, 5, 16):
            with self.subTest(scenario=scenario):
                out = self.run_case(scenario)
                self.assertEqual((out[0], out[2], out[3], out[4], out[5]), (7, 0, 1, 0, 1))
                self.assertEqual(out[12:14], [0, 0])

    def test_untracked_doorbell_task_queue_or_disabled_hce_refuses_submission(self):
        for scenario in (6, 7, 8):
            out = self.run_case(scenario)
            self.assertEqual((out[0], out[3], out[8], out[5]), (7, 0, 0, 1))

    def test_queue_stop_timeout_is_bounded_and_base_write_failure_does_not_ring(self):
        out = self.run_case(9)
        self.assertEqual((out[0], out[3], out[9]), (18, 0, 100000))
        out = self.run_case(10)
        self.assertEqual((out[0], out[3], out[8]), (7, 0, 0))

    def test_partial_sync_failure_retains_success_but_never_replays_failed_lun(self):
        out = self.run_case(11)
        self.assertEqual((out[0], out[3], out[4], out[12], out[13], out[5]), (7, 2, 1, 1, 0, 1))
        self.assertEqual(out[14:17], [7, out[7], out[10]])

    def test_counter_overflow_and_allocation_or_mapping_failure_submit_nothing(self):
        for scenario, result in ((12, 7), (14, 9), (15, 9)):
            out = self.run_case(scenario)
            self.assertEqual((out[0], out[3], out[8], out[5]), (result, 0, 0, 1))

    def test_failed_pending_query_or_retirement_stops_before_sync(self):
        for scenario in (17, 18):
            out = self.run_case(scenario)
            self.assertEqual((out[0], out[3], out[5], out[8]), (7, 0, 1, 0))

    def test_never_initialized_controller_needs_no_mmio_or_dma(self):
        out = self.run_case(20)
        self.assertEqual((out[0], out[2], out[7], out[10], out[27]), (0, 1, 0, 0, 0))

    def test_no_learned_lun_only_closes_idle_queues_without_guessing_lun_zero(self):
        out = self.run_case(21)
        self.assertEqual((out[0], out[2], out[3], out[4], out[8], out[10]), (0, 1, 0, 0, 0, 0))
