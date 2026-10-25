"""Execute unchanged production PassThru function bodies with transport faults."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from mu_patch_sources import patched_mu_source


def function(source, name):
    match = re.search(r'(?m)^(?:STATIC\s+)?(?:BOOLEAN|EFI_STATUS)\s+(?:EFIAPI\s+)?' + name + r'\s*\(', source)
    if not match:
        raise AssertionError('production function missing: ' + name)
    at = source.index('{', match.end())
    depth = 1
    end = at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


class PassThruPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        root = Path(__file__).resolve().parents[2]
        with patched_mu_source(root) as mu:
            source = (mu / 'MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruHci.c').read_text()
            interface = (mu / 'MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThru.c').read_text()
        names = ('UfsHasCommandPolicy', 'UfsCommandPolicy', 'UfsWaitMemSet', 'UfsStartExecCmd',
                 'UfsStopExecCmd', 'UfsFinishCommand', 'UfsSendDmRequestRetry', 'UfsSendDmRequest',
                 'UfsExecNopCmds', 'UfsExecScsiCmds')
        header = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;
typedef uint64_t UINT64;typedef size_t UINTN;typedef bool BOOLEAN;
typedef uint64_t EFI_STATUS;typedef unsigned EFI_TPL;typedef void *EFI_EVENT,*EFI_HANDLE;
#define STATIC static
#define VOID void
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_NOT_READY 6
#define EFI_DEVICE_ERROR 7
#define EFI_WRITE_PROTECTED 8
#define EFI_OUT_OF_RESOURCES 9
#define EFI_TIMEOUT 18
#define EFI_SECURITY_VIOLATION 26
#define EFI_ERROR(s) ((s)!=0)
#define BIT0 1
#define BIT5 32
#define BIT7 128
#define TARGET_MAX_BYTES 16
#define UFS_MAX_LUNS 8
#define UFS_WLUN_PREFIX 0xc1
#define EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_NONBLOCKIO 4
#define ADDRESS_IS_ALIGNED(p,n) (((uintptr_t)(p) % (n)) == 0)
#define SetMem(p,n,v) memset(p,v,n)
#define ZeroMem(p,n) memset(p,0,n)
#define CompareMem memcmp
#define TPL_NOTIFY 16
#define UFS_TIMEOUT 30
#define UFS_HC_UTRLDBR_OFFSET 0x58
#define UFS_HC_UTRLCLR_OFFSET 0x5c
#define UFS_HC_UTRLRSR_OFFSET 0x60
#define UFS_HC_UTRLRSR 1
#define EDKII_UFS_HC_PLATFORM_COMMAND_POLICY_VERSION 4
#define EFI_EXT_SCSI_DATA_DIRECTION_READ 0
#define EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OTHER 0x7f
#define EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK 0
#define EFI_EXT_SCSI_STATUS_TARGET_GOOD 0
#define EFI_EXT_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND 9
#define UFS_PASS_THRU_TRANS_REQ_SIG 123
#define UfsUtpQueryResponseSuccess 0
#define UfsUtpQueryResponseInvalidSelector 0xfb
#define UfsUtpQueryResponseInvalidIndex 0xfc
#define UfsUtpQueryResponseInvalidIdn 0xfd
#define EFI_SIZE_TO_PAGES(n) (((n)+4095)/4096)
#define DEBUG(x) do{}while(0)
#define ASSERT(x) do{}while(0)
#define CopyMem memcpy
typedef struct {UINT8 Flags,Response,Status;UINT16 SenseDataLen;UINT32 ResTranCount;UINT8 SenseData[18];} UTP_RESPONSE_UPIU;
typedef struct {UINT8 QueryResp;} UTP_QUERY_RESP_UPIU;
typedef struct {UINT8 Resp;} UTP_NOP_IN_UPIU;
typedef struct {UINT32 RuO,RuL,PrdtO,PrdtL,Ocs;} UTP_TRD;
typedef struct {UINT32 Dummy;} UTP_TR_PRD;
typedef struct {UINT64 Timeout;UINT8 *Cdb,CdbLength,DataDirection,TargetStatus,SenseDataLength,HostAdapterStatus;UINT32 InTransferLength,OutTransferLength;void *SenseData,*InDataBuffer,*OutDataBuffer;} EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET;
typedef struct {UINT8 Opcode,DescId,Index,Selector,DataDirection;UINT32 TransferLength;UINT64 Timeout;} UFS_DEVICE_MANAGEMENT_REQUEST_PACKET;
typedef enum {EdkiiUfsHcValidateScsi,EdkiiUfsHcCompleteScsi,EdkiiUfsHcValidateQuery,EdkiiUfsHcAbort} EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE;
typedef struct {UINT8 Lun;EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet;EFI_EVENT Event;} EDKII_UFS_HC_SCSI_POLICY;
typedef struct {UINT8 Opcode,Idn,Index,Selector,DataDirection;UINT32 TransferLength;UINT64 Timeout;} EDKII_UFS_HC_QUERY_POLICY;
typedef struct {int dummy;} LIST_ENTRY;
typedef struct HC HC;
struct HC {EFI_STATUS (*Flush)(HC*);EFI_STATUS (*Unmap)(HC*,void*);EFI_STATUS (*FreeBuffer)(HC*,UINTN,void*);};
typedef HC EDKII_UFS_HOST_CONTROLLER_PROTOCOL;
typedef struct {void *Handle;HC *UfsHostController;void *UtpTrlBase;LIST_ENTRY Queue;struct {UINT8 BitMask,Lun[8];} Luns;} UFS_PASS_THRU_PRIVATE_DATA;
typedef struct {UINT32 IoAlign,Attributes;} EFI_EXT_SCSI_PASS_THRU_MODE;
typedef struct {EFI_EXT_SCSI_PASS_THRU_MODE *Mode;UFS_PASS_THRU_PRIVATE_DATA *Private;} EFI_EXT_SCSI_PASS_THRU_PROTOCOL;
#define UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS(p) ((p)->Private)
static UINT8 mUfsTargetId[TARGET_MAX_BYTES];
typedef struct {unsigned Signature;UINT64 TimeoutRemain;EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet;
 UINT8 Slot;UTP_TRD *Trd;void *CmdDescHost,*CmdDescMapping;UINT32 CmdDescSize;EFI_EVENT CallerEvent;LIST_ENTRY TransferList;} UFS_PASS_THRU_TRANS_REQ;
typedef struct {unsigned Version;EFI_STATUS (*Callback)(void*,EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE,void*);} PLATFORM;
static PLATFORM platform,*mUfsHcPlatform=&platform;
static unsigned scenario,kind,starts,aborts,unmaps,freebufs,reconciles,pools,poolfrees,ctors,querycopies,completes,reads;
static unsigned doorbell;static void *pool;
static unsigned char descriptor[128];static UTP_TRD trd;
static EFI_STATUS callback(void *h,EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE phase,void *data){
 (void)h;
 if(phase==EdkiiUfsHcAbort){aborts++;if(scenario==3)return EFI_DEVICE_ERROR;doorbell=0;return 0;}
 if(phase==EdkiiUfsHcCompleteScsi){completes++;return 0;}
 if(scenario==4)return EFI_WRITE_PROTECTED;
 if(phase==EdkiiUfsHcValidateScsi){EDKII_UFS_HC_SCSI_POLICY *p=data;
  if(p->Event||!p->Packet->Timeout||p->Packet->Timeout>UFS_TIMEOUT)return EFI_INVALID_PARAMETER;}
 return 0;
}
static EFI_STATUS flush(HC *h){(void)h;return 0;}
static EFI_STATUS unmap(HC *h,void *m){(void)h;(void)m;unmaps++;return scenario==10?EFI_DEVICE_ERROR:0;}
static EFI_STATUS release(HC *h,UINTN n,void *m){(void)h;(void)m;(void)n;freebufs++;return scenario==11?EFI_DEVICE_ERROR:0;}
static HC hc={flush,unmap,release};
static EFI_TPL raise(unsigned t){return t;}static void restore(EFI_TPL t){(void)t;}
static struct {EFI_TPL (*RaiseTPL)(unsigned);void (*RestoreTPL)(EFI_TPL);} bs={raise,restore},*gBS=&bs;
static void InsertTailList(LIST_ENTRY *a,LIST_ENTRY *b){(void)a;(void)b;}
static UINT64 DivU64x32(UINT64 a,UINT32 b){return a/b;}
static void MicroSecondDelay(unsigned n){(void)n;}
static void *AllocateZeroPool(UINTN n){pools++;pool=calloc(1,n);return pool;}
static void FreePool(void *p){poolfrees++;free(p);if(pool==p)pool=NULL;}
static EFI_STATUS UfsMmioRead32(UFS_PASS_THRU_PRIVATE_DATA *p,UINTN offset,UINT32 *v){
 (void)p;if(offset==UFS_HC_UTRLDBR_OFFSET){reads++;if(scenario==14&&reads>1)return EFI_DEVICE_ERROR;*v=doorbell;}
 else *v=offset==UFS_HC_UTRLRSR_OFFSET?1:0;return 0;
}
static EFI_STATUS UfsMmioWrite32(UFS_PASS_THRU_PRIVATE_DATA *p,UINTN offset,UINT32 v){
 (void)p;(void)v;if(offset==UFS_HC_UTRLDBR_OFFSET){starts++;if(scenario==1)return EFI_DEVICE_ERROR;
  doorbell=(scenario==2||scenario==3||scenario==15)?1:0;}return 0;
}
static EFI_STATUS UfsFindAvailableSlotInTrl(UFS_PASS_THRU_PRIVATE_DATA *p,UINT8 *slot){(void)p;*slot=0;return scenario==12?EFI_NOT_READY:0;}
static EFI_STATUS create(UTP_TRD *t,void **host,void **mapping){
 ctors++;if(scenario==13)return EFI_OUT_OF_RESOURCES;*host=descriptor;*mapping=(void*)1;t->RuO=0;t->RuL=8;t->PrdtO=1;t->PrdtL=0;t->Ocs=scenario==16?1:0;
 memset(descriptor,0,sizeof descriptor);
 if(kind==1){((UTP_QUERY_RESP_UPIU*)descriptor)->QueryResp=scenario==7?0xfd:0;}
 if(kind==0){UTP_RESPONSE_UPIU *r=(void*)descriptor;
  if(scenario==8){r->Flags=BIT5;r->ResTranCount=0x09000000;}
  if(scenario==9)r->SenseDataLen=0x1300;
 }return 0;
}
static EFI_STATUS UfsCreateDMCommandDesc(UFS_PASS_THRU_PRIVATE_DATA *p,UFS_DEVICE_MANAGEMENT_REQUEST_PACKET *q,UTP_TRD *t,void **host,void **mapping){(void)p;(void)q;return create(t,host,mapping);}
static EFI_STATUS UfsCreateNopCommandDesc(UFS_PASS_THRU_PRIVATE_DATA *p,UTP_TRD *t,void **host,void **mapping){(void)p;return create(t,host,mapping);}
static EFI_STATUS UfsCreateScsiCommandDesc(UFS_PASS_THRU_PRIVATE_DATA *p,UINT8 lun,EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *q,UTP_TRD *t,void **host,void **mapping){(void)p;(void)q;(void)lun;return create(t,host,mapping);}
static EFI_STATUS UfsPrepareDataTransferBuffer(UFS_PASS_THRU_PRIVATE_DATA *p,UFS_PASS_THRU_TRANS_REQ *r){(void)p;(void)r;return 0;}
static EFI_STATUS UfsReconcileDataTransferBuffer(UFS_PASS_THRU_PRIVATE_DATA *p,UFS_PASS_THRU_TRANS_REQ *r){(void)p;(void)r;reconciles++;return scenario==10?EFI_DEVICE_ERROR:0;}
static EFI_STATUS UfsGetReturnDataFromQueryResponse(UFS_DEVICE_MANAGEMENT_REQUEST_PACKET *p,UTP_QUERY_RESP_UPIU *r){(void)p;(void)r;querycopies++;return 0;}
static void DumpQueryResponseResult(UINT8 r){(void)r;}
static void SwapLittleEndianToBigEndian(UINT8 *p,UINT32 n){for(unsigned i=0;i<n/2;i++){UINT8 t=p[i];p[i]=p[n-1-i];p[n-1-i]=t;}}
'''
        footer = r'''
void run(unsigned mode,unsigned fault,uint64_t *out){
 kind=mode>=3?0:mode;scenario=fault;starts=aborts=unmaps=freebufs=reconciles=pools=poolfrees=ctors=querycopies=completes=reads=doorbell=0;pool=NULL;
 platform=(PLATFORM){4,fault==18?NULL:callback};memset(&trd,0,sizeof trd);
 UFS_PASS_THRU_PRIVATE_DATA p={0};p.UfsHostController=&hc;p.UtpTrlBase=&trd;
 unsigned char sense[32]={0},cdb[16]={0};
 EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET packet={0};packet.Timeout=scenario==6?0:10;packet.InTransferLength=8;
 packet.Cdb=cdb;packet.CdbLength=10;packet.DataDirection=0;packet.SenseData=sense;packet.SenseDataLength=32;
 UFS_DEVICE_MANAGEMENT_REQUEST_PACKET query={0};query.Opcode=1;query.Timeout=10;
 if(mode>=3){
  EFI_EXT_SCSI_PASS_THRU_MODE advertised={1,mode==4?EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_NONBLOCKIO:0};
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL protocol={&advertised,&p};UINT8 target[TARGET_MAX_BYTES]={0};
  p.Luns.BitMask=1;p.Luns.Lun[0]=2;
  out[0]=UfsPassThruPassThru(&protocol,target,0x200,&packet,(void*)1);
 }else out[0]=mode==0?UfsExecScsiCmds(&p,2,&packet,scenario==5?(void*)1:NULL):
        (mode==1?UfsSendDmRequest(&p,&query):UfsExecNopCmds(&p));
 out[1]=starts;out[2]=aborts;out[3]=unmaps;out[4]=freebufs;out[5]=reconciles;out[6]=pools;out[7]=poolfrees;
 out[8]=ctors;out[9]=packet.InTransferLength;out[10]=completes;out[11]=querycopies;out[12]=reads;out[13]=packet.SenseDataLength;
 out[14]=packet.HostAdapterStatus;out[15]=packet.TargetStatus;
 if(pool)free(pool);pool=NULL;
}
'''
        (base / 'test.c').write_text(header + '\n\n'.join(function(source, name) for name in names) +
                                   '\n' + function(interface, 'UfsPassThruPassThru') + footer)
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(base / 'test.c'), '-o', str(base / 'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'test.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_case(self, mode, fault):
        out = (ctypes.c_uint64 * 20)(); self.lib.run(mode, fault, out)
        return list(out)

    def test_all_three_command_paths_check_start_failure_and_abort_before_cleanup(self):
        for mode in range(3):
            out = self.run_case(mode, 1)
            self.assertEqual(out[:5], [7, 1, 1, 1, 1])

    def test_timeout_stops_then_reclaims_and_never_retries_query(self):
        for mode in range(3):
            out = self.run_case(mode, 2)
            self.assertEqual(out[:5], [18, 1, 1, 1, 1])
            self.assertEqual(out[8], 1)

    def test_failed_stop_retains_descriptors_and_scsi_request_without_reconcile(self):
        for mode in range(3):
            out = self.run_case(mode, 3)
            self.assertEqual(out[:6], [7, 1, 1, 0, 0, 0])
            self.assertEqual(out[7], 0)

    def test_async_zero_timeout_and_rejected_commands_never_allocate(self):
        for fault in (4, 5, 6):
            out = self.run_case(0, fault)
            self.assertNotEqual(out[0], 0)
            self.assertEqual(out[1:9], [0] * 8)
        out = self.run_case(1, 4)
        self.assertEqual(out[:5], [8, 0, 0, 0, 0])

    def test_device_query_error_is_returned_once_without_abort_or_data_copy(self):
        out = self.run_case(1, 7)
        self.assertEqual(out[:5], [2, 1, 0, 1, 1])
        self.assertEqual((out[8], out[11]), (1, 0))

    def test_residual_underflow_and_oversize_sense_cannot_escape(self):
        out = self.run_case(0, 8)
        self.assertEqual((out[0], out[9], out[10]), (7, 0, 0))
        self.assertEqual(self.run_case(0, 9)[13], 0)

    def test_cleanup_failures_are_reported(self):
        for mode in range(3):
            for fault in (10, 11):
                self.assertEqual(self.run_case(mode, fault)[0], 7)

    def test_early_allocation_failures_do_not_leak_scsi_request(self):
        for fault in (12, 13):
            out = self.run_case(0, fault)
            self.assertNotEqual(out[0], 0)
            self.assertEqual(out[6:8], [1, 1])

    def test_stop_read_error_requires_abort_before_free(self):
        for mode in range(3): self.assertEqual(self.run_case(mode, 14)[:5], [0, 1, 1, 1, 1])

    def test_nop_ocs_failure_is_an_error(self):
        self.assertEqual(self.run_case(2, 16)[0], 7)

    def test_host_status_is_explicit_for_completion_timeout_and_rejection(self):
        self.assertEqual(self.run_case(0, 0)[14:16], [0, 0])
        self.assertEqual(self.run_case(0, 2)[14], 9)
        self.assertEqual(self.run_case(0, 4)[14:16], [0x7f, 0xff])

    def test_direct_passthru_ignores_event_when_nonblocking_is_not_advertised(self):
        self.assertEqual(self.run_case(3, 0)[:5], [0, 1, 0, 1, 1])
        self.assertEqual(self.run_case(4, 0)[:5], [2, 0, 0, 0, 0])

    def test_v4_policy_without_callback_fails_closed_before_allocation(self):
        out = self.run_case(0, 18)
        self.assertEqual(out[0], 26)
        self.assertEqual(out[1:9], [0] * 8)
