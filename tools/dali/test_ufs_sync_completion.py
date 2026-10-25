"""Count sync only at the production host's validated hardware completion."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_ufs_passthru_policy as passthru


class UfsSyncCompletionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler: raise unittest.SkipTest('C compiler required')
        cls.temp = tempfile.TemporaryDirectory(); base = Path(cls.temp.name)
        source = (Path(__file__).resolve().parents[2] / 'Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe.c').read_text()
        header = r'''
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t UINT8;typedef uint64_t UINT64,EFI_STATUS;typedef bool BOOLEAN;typedef char CHAR8;
#define STATIC static
#define VOID void
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 7
#define EFI_ERROR(s) ((s)!=0)
#define MAX_UINT64 UINT64_MAX
#define HCI 0x16810000U
#define DEBUG_WARN 1
#define FixedPcdGetPtr(p) "token"
#define DEBUG(args) debug args
static void debug(int lvl,const char *fmt,...){(void)lvl;(void)fmt;}
static struct {UINT8 Transaction,Lun,Tag,Opcode;UINT8 *Request;} mPending;
static struct {BOOLEAN Busy;} mDma;
static unsigned mGeometry[8],fault,completed,stopped;static volatile UINT64 mCompletedSync[8];
static void MemoryFence(void){}
static unsigned MmioRead32(unsigned address){(void)address;return fault==5?4:0;}
static EFI_STATUS Mt6991UfsValidateResponse(void *p,void *g,BOOLEAN *valid){(void)p;(void)g;*valid=fault!=2&&fault!=3;return fault==4?7:0;}
static EFI_STATUS Mt6991UfsDmaComplete(void *d,BOOLEAN valid){(void)d;(void)valid;completed++;return fault==6?7:0;}
static EFI_STATUS StopController(void){stopped++;return 0;}
'''
        footer = r'''
void run(unsigned which,UINT64 *out){
 fault=which;completed=stopped=0;for(unsigned i=0;i<8;i++)mCompletedSync[i]=0;
 UINT8 request[32]={0};request[16]=which==8?0x25:0x35;
 mPending.Transaction=which==2?0x16:1;mPending.Lun=2;mPending.Tag=17;mPending.Opcode=which==1?0x25:0x35;mPending.Request=request;
 if(which==7)mCompletedSync[2]=UINT64_MAX;
 out[0]=Complete();out[1]=completed;out[2]=stopped;out[3]=mCompletedSync[2];
}
'''
        (base/'test.c').write_text(header + passthru.function(source, 'Complete') + footer)
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(base/'test.c'), '-o', str(base/'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(base/'test.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def case(self, fault):
        out = (ctypes.c_uint64 * 4)(); self.lib.run(fault, out)
        return list(out)

    def test_only_the_previously_submitted_sync_opcode_counts(self):
        self.assertEqual(self.case(0), [0, 1, 0, 1])
        self.assertEqual(self.case(1), [0, 1, 0, 0])
        self.assertEqual(self.case(8), [0, 1, 0, 1])

    def test_noncommand_or_check_condition_does_not_count(self):
        for fault in (2, 3): self.assertEqual(self.case(fault), [0, 1, 0, 0])

    def test_invalid_response_interrupt_or_dma_completion_stops_host_without_counting(self):
        for fault in (4, 5): self.assertEqual(self.case(fault), [7, 0, 1, 0])
        self.assertEqual(self.case(6), [7, 1, 1, 0])

    def test_counter_never_wraps_into_false_evidence(self):
        self.assertEqual(self.case(7), [7, 1, 1, 2**64-1])
