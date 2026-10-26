"""Execute the production provider envelope; shutdown/DMA engines tested separately."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class UfsQuiesceAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            raise unittest.SkipTest('native C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        root = Path(__file__).resolve().parents[2]
        source = (root/'Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe.c').read_text()
        # Balanced production function bodies, never replacements of their logic.
        functions = []
        for name in ('StopController', 'Quiesce', 'StorageQuiesce', 'Allocate', 'Map', 'WriteLocked', 'Write'):
            match = re.search(r'STATIC EFI_STATUS (?:EFIAPI )?' + name + r'\s*\([^;{]*\)\s*\{', source)
            if not match:
                raise AssertionError('missing production function '+name)
            at, depth = match.end(), 1
            while depth:
                depth += (source[at] == '{') - (source[at] == '}')
                at += 1
            functions.append(source[match.start():at])
        prefix = '''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
typedef uint64_t EFI_STATUS;typedef unsigned long UINTN;typedef uint64_t UINT64;
typedef uint32_t UINT32;typedef char CHAR8;typedef bool BOOLEAN;typedef void VOID;
typedef void *EFI_HANDLE;typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef unsigned EFI_ALLOCATE_TYPE;typedef unsigned EFI_MEMORY_TYPE;
typedef unsigned EFI_TPL;typedef unsigned EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH;
#define STATIC static
#define EFIAPI
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0ULL
#define E(n) (0x8000000000000000ULL|(n))
#define EFI_INVALID_PARAMETER E(2)
#define EFI_UNSUPPORTED E(3)
#define EFI_DEVICE_ERROR E(7)
#define EFI_NOT_READY E(6)
#define EFI_ACCESS_DENIED E(15)
#define EFI_NOT_FOUND E(14)
#define EFI_TIMEOUT E(18)
#define EFI_ERROR(n) (((uint64_t)(n)>>63)!=0)
#define DEBUG(x) ((void)0)
#define FixedPcdGetPtr(x) "token"
#define HCI 0x16810000U
#define WINDOW 0x2a00U
#define TPL_NOTIFY 16
#define TPL_APPLICATION 4
#define EfiUfsHcWidthUint32 2
#define CopyMem memcpy
#define MemoryFence() ((void)0)
#define EfiBootServicesData 4
#define EdkiiUfsHcOperationMaximum 3
#define Mt6991UfsStopped 8
typedef unsigned EDKII_UFS_HOST_CONTROLLER_OPERATION;
typedef unsigned MT6991_DMA_OPERATION;
typedef struct {unsigned unused;} EDKII_UFS_HOST_CONTROLLER_PROTOCOL;
typedef struct {unsigned unused;} MT6991_UFS_SCRATCH_GRANT;
typedef struct {bool Initialized,Poisoned,Stopped,Busy;} DMA;
typedef struct {bool Touched;unsigned Io;} CONTROL;
typedef struct {bool Attempted,Completed;unsigned SyncedLuns,Commands;EFI_STATUS Status;} SHUTDOWN;
typedef struct {unsigned unused;} UNIXNU_STORAGE_QUIESCE_PROTOCOL;
static DMA mDma;static CONTROL mControl;static SHUTDOWN mShutdown;static MT6991_UFS_SCRATCH_GRANT mScratchGrant;
static struct {unsigned Transaction;unsigned char *Request;} mPending;
static unsigned mGeometry[8],mTrl,mTmrl;static volatile uint64_t mCompletedSync[8];
static bool mClosed,mDisconnected,mQuiesced,mQuiescing,mInitUsed,mInitFinished;static EFI_STATUS mQuiesceFailure;
static EFI_HANDLE mController;static EDKII_UFS_HOST_CONTROLLER_PROTOCOL mHost;
static UNIXNU_STORAGE_QUIESCE_PROTOCOL mStorage;
static unsigned gEfiExtScsiPassThruProtocolGuid;
static unsigned scenario,used,trace[64],sync_commands,stop_calls,disconnect_calls,release_calls,probe_calls,alloc_calls,map_calls;
static unsigned hce,tpl,mmio_writes,submit_calls,validate_calls;
static bool raise_hook;static EFI_STATUS reentrant_status;
static EFI_TPL EfiGetCurrentTpl(void){return tpl;}
static void add(unsigned n) {if(used<64)trace[used++]=n;}
static EFI_STATUS DaliUfsScratchCloseAdmission(void){if(tpl!=TPL_NOTIFY)abort();return scenario==15?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS DaliUfsScratchUnpublish(void){if(tpl!=TPL_APPLICATION||!mClosed)abort();add(7);return scenario==14?EFI_ACCESS_DENIED:EFI_SUCCESS;}
static bool ValidHost(EDKII_UFS_HOST_CONTROLLER_PROTOCOL *p){return p==&mHost;}
static EFI_STATUS EFIAPI Quiesce(void);
static EFI_TPL raise_tpl(EFI_TPL n){if(n<tpl)abort();if(raise_hook){raise_hook=false;if(Quiesce()!=0)abort();}unsigned old=tpl;tpl=n;return old;}
static void restore_tpl(EFI_TPL n){if(n>tpl)abort();tpl=n;}
static uint32_t MmioRead32(size_t a){return a==HCI+0x34?hce:a==HCI+0x60?1:0;}
static void MmioWrite32(size_t a,uint32_t n){(void)a;(void)n;if(tpl!=TPL_NOTIFY)abort();++mmio_writes;}
static void Mt6991UfsDmaPoison(DMA *d){d->Poisoned=true;}
static unsigned Mt6991UfsStop(CONTROL *c){(void)c;if(tpl!=TPL_NOTIFY)abort();++stop_calls;add(2);if(scenario==2&&stop_calls==1)return 3;if(scenario!=9)hce=0;return 8;}
static EFI_STATUS Mt6991UfsDmaStopped(DMA *d,uint32_t enabled){add(3);if(enabled!=0)return EFI_NOT_READY;d->Stopped=true;return 0;}
static EFI_STATUS Mt6991UfsDmaReleaseAll(DMA *d){if(tpl!=TPL_NOTIFY)abort();++release_calls;add(6);if(!d->Stopped||hce)return EFI_DEVICE_ERROR;return scenario==6&&release_calls==1?EFI_DEVICE_ERROR:0;}
static EFI_STATUS Mt6991UfsDmaResolve(DMA *d,UINT64 a,UINTN n,bool common,void **p){(void)d;(void)a;(void)n;(void)common;(void)p;return 0;}
static EFI_STATUS Mt6991UfsValidateTransfer(DMA *d,UINT64 a,void *g,void *grant,bool init,void *p){(void)d;(void)a;(void)g;(void)grant;(void)init;(void)p;++validate_calls;return 0;}
static EFI_STATUS Mt6991UfsDmaSubmit(DMA *d){if(tpl!=TPL_NOTIFY)abort();d->Busy=true;++submit_calls;return 0;}
static EFI_STATUS Mt6991UfsDmaAllocate(DMA *d,UINTN n,void **h){(void)d;(void)n;(void)h;++alloc_calls;return 0;}
static EFI_STATUS Mt6991UfsDmaMap(DMA *d,unsigned op,void *h,UINTN n,UINT64 *dev,UINT64 *id){(void)d;(void)op;(void)h;(void)n;(void)dev;(void)id;++map_calls;return 0;}
static EFI_STATUS disconnect(EFI_HANDLE h,void *a,void *b){(void)h;(void)a;(void)b;if(tpl!=4)abort();++disconnect_calls;add(4);return scenario==4?1:scenario==7?EFI_NOT_FOUND:0;}
static EFI_STATUS protocol(EFI_HANDLE h,void *guid,void **p){(void)h;(void)guid;(void)p;++probe_calls;add(5);return scenario==3?0:scenario==5?EFI_DEVICE_ERROR:EFI_UNSUPPORTED;}
static struct {EFI_STATUS(*DisconnectController)(EFI_HANDLE,void*,void*);EFI_STATUS(*HandleProtocol)(EFI_HANDLE,void*,void**);EFI_TPL(*RaiseTPL)(EFI_TPL);void(*RestoreTPL)(EFI_TPL);} bs={disconnect,protocol,raise_tpl,restore_tpl},*gBS=&bs;
static EFI_STATUS Allocate(EDKII_UFS_HOST_CONTROLLER_PROTOCOL*,unsigned,unsigned,UINTN,void**,UINT64);
static EFI_STATUS Map(EDKII_UFS_HOST_CONTROLLER_PROTOCOL*,unsigned,void*,UINTN*,UINT64*,void**);
static EFI_STATUS Write(EDKII_UFS_HOST_CONTROLLER_PROTOCOL*,unsigned,UINT64,UINTN,void*);
static EFI_STATUS StorageQuiesce(UNIXNU_STORAGE_QUIESCE_PROTOCOL*);
static EFI_STATUS DaliUfsDrainAndSync(DMA *d,void *p,unsigned *g,volatile uint64_t *c,unsigned *io,SHUTDOWN *s){
 (void)d;(void)p;(void)g;(void)c;(void)io;
 if(s->Attempted)return s->Status;
 add(1);s->Attempted=true;++sync_commands;s->Commands=1;
 void *host=NULL,*mapping=(void*)1;size_t bytes=32;uint64_t dev=0;
 if(!mClosed||Allocate(&mHost,0,4,1,&host,0)!=EFI_DEVICE_ERROR||Map(&mHost,0,(void*)1,&bytes,&dev,&mapping)!=EFI_DEVICE_ERROR||mapping!=NULL)return EFI_INVALID_PARAMETER;
 uint32_t doorbell=1;
 if(Write(&mHost,EfiUfsHcWidthUint32,0x58,1,&doorbell)!=EFI_DEVICE_ERROR||submit_calls||mmio_writes||validate_calls||tpl!=TPL_NOTIFY)return EFI_INVALID_PARAMETER;
 if(scenario==11)reentrant_status=StorageQuiesce(&mStorage);
 s->Status=scenario==1?EFI_TIMEOUT:scenario==8?1:EFI_SUCCESS;
 s->Completed=s->Status==0;s->SyncedLuns=s->Completed?5:0;return s->Status;
}
'''
        suffix = '''
void run(unsigned s,uint64_t *out){
 scenario=s;used=sync_commands=stop_calls=disconnect_calls=release_calls=probe_calls=alloc_calls=map_calls=0;
 memset(trace,0,sizeof trace);memset(&mShutdown,0,sizeof mShutdown);memset(&mControl,0,sizeof mControl);
 mDma=(DMA){true,false,false,false};mClosed=mDisconnected=mQuiesced=mQuiescing=false;mQuiesceFailure=0;mController=(void*)1;
 hce=1;tpl=4;mmio_writes=submit_calls=validate_calls=0;raise_hook=false;reentrant_status=0;mTrl=0x10000000;
 if(s==10){raise_hook=true;uint32_t doorbell=1;out[0]=Write(&mHost,EfiUfsHcWidthUint32,0x58,1,&doorbell);}
 else {if(s==12)tpl=16;if(s==13)tpl=31;out[0]=StorageQuiesce(&mStorage);}
 out[1]=mClosed;out[2]=mQuiesced;out[3]=mQuiesceFailure;
 out[4]=sync_commands;out[5]=stop_calls;out[6]=disconnect_calls;out[7]=probe_calls;out[8]=release_calls;
 out[9]=alloc_calls;out[10]=map_calls;out[11]=used;for(unsigned i=0;i<used;i++)out[12+i]=trace[i];
 out[80]=StorageQuiesce(&mStorage);out[81]=sync_commands;out[82]=stop_calls;out[83]=disconnect_calls;
 out[84]=release_calls;out[85]=mQuiesced;out[86]=StorageQuiesce(NULL);
 out[87]=mmio_writes;out[88]=submit_calls;out[89]=validate_calls;out[90]=tpl;out[91]=reentrant_status;out[92]=mQuiescing;
}
'''
        (base/'envelope.c').write_text(prefix+'\n'.join(functions)+suffix)
        subprocess.run([cc, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(base/'envelope.c'), '-o', str(base/'adapter.so')], check=True)
        cls.lib = ctypes.CDLL(str(base/'adapter.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def case(self, scenario=0):
        out = (ctypes.c_uint64*96)()
        self.lib.run(scenario, out)
        return list(out)

    def test_close_drain_sync_stop_disconnect_verify_then_release_and_idempotent_success(self):
        out = self.case()
        self.assertEqual(out[:11], [0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0])
        self.assertEqual(out[12:19], [1, 2, 3, 7, 4, 5, 6])
        self.assertEqual(out[80:86], [0, 1, 1, 1, 1, 1])
        self.assertEqual(out[86], (1<<63)|2)

    def test_drain_failure_still_stops_and_cleans_but_never_authorizes_handoff(self):
        out = self.case(1)
        self.assertEqual(out[0], (1<<63)|18)
        self.assertEqual(out[12:19], [1, 2, 3, 7, 4, 5, 6])
        self.assertEqual((out[80],out[81],out[85]), ((1<<63)|18,1,0))

    def test_stop_failure_never_releases_and_later_cleanup_does_not_clear_first_failure(self):
        out = self.case(2)
        self.assertEqual((out[0],out[6],out[8]), ((1<<63)|7,0,0))
        self.assertEqual((out[80],out[81],out[84],out[85]), ((1<<63)|7,1,1,0))

    def test_present_protocol_or_uncertain_disconnect_cannot_release_dma(self):
        for scenario in (3,4,5):
            out = self.case(scenario)
            self.assertEqual((out[0],out[8],out[85]), ((1<<63)|7,0,0))

    def test_release_failure_can_retry_cleanup_without_sync_replay_or_failure_promotion(self):
        out = self.case(6)
        self.assertEqual((out[0],out[80],out[81],out[83],out[84],out[85]), ((1<<63)|7,(1<<63)|7,1,1,2,0))

    def test_already_disconnected_requires_protocol_absence_and_warning_sync_fails_closed(self):
        self.assertEqual(self.case(7)[0], 0)
        out = self.case(8)
        self.assertEqual((out[0],out[80],out[85]), ((1<<63)|7,(1<<63)|7,0))

    def test_hce_still_enabled_after_stop_never_disconnects_or_releases(self):
        out = self.case(9)
        self.assertEqual((out[0],out[6],out[8],out[80],out[84],out[85]), ((1<<63)|6,0,0,(1<<63)|6,0,0))

    def test_close_at_public_write_entry_cannot_validate_submit_or_ring(self):
        out = self.case(10)
        self.assertEqual((out[0],out[2],out[4],out[87],out[88],out[89],out[90]), ((1<<63)|7,1,1,0,0,0,4))

    def test_reentrant_quiesce_refuses_without_repeating_shutdown(self):
        out = self.case(11)
        self.assertEqual((out[0],out[4],out[5],out[91],out[92],out[90]), (0,1,1,(1<<63)|6,0,4))

    def test_non_application_caller_is_refused_before_close_or_hardware(self):
        for scenario, tpl in ((12, 16), (13, 31)):
            out = self.case(scenario)
            self.assertEqual((out[0],out[1],out[4],out[5],out[8],out[90]), ((1<<63)|3,0,0,0,0,tpl))

    def test_failed_scratch_unpublish_is_sticky_but_still_completes_safe_dma_cleanup(self):
        out = self.case(14)
        self.assertEqual((out[0],out[2],out[4],out[6],out[8]), ((1<<63)|15,0,1,1,1))
        self.assertEqual((out[80],out[81],out[85]), ((1<<63)|15,1,0))
        self.assertEqual(out[12:19], [1,2,3,7,4,5,6])

    def test_busy_scratch_refuses_before_closing_host_or_stopping_its_completion_path(self):
        out = self.case(15)
        self.assertEqual((out[0],out[1],out[2],out[4],out[5],out[6],out[8]), ((1<<63)|6,0,0,0,0,0,0))
        self.assertEqual((out[80],out[92]), ((1<<63)|6,0))
