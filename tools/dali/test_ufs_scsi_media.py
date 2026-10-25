"""Execute production ScsiDisk media policy and both real synchronization APIs."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_ufs_passthru_policy as passthru
from mu_patch_sources import patched_mu_source


class UfsScsiMediaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler: raise unittest.SkipTest('C compiler required')
        cls.temp = tempfile.TemporaryDirectory(); base = Path(cls.temp.name)
        root = Path(__file__).resolve().parents[2]
        with patched_mu_source(root) as mu:
            source = (mu / 'MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDisk.c').read_text()
        names = ('ScsiDiskApplyUfsMediaPolicy', 'ScsiDiskUfsSynchronizeCache', 'ScsiDiskFlushBlocks', 'ScsiDiskFlushBlocksEx')
        header = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef size_t UINTN;typedef bool BOOLEAN;typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE,*EFI_EVENT;typedef unsigned EFI_TPL;typedef int EFI_GUID;
#define STATIC static
#define VOID void
#define EFIAPI
#define IN
#define OUT
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_NOT_READY 6
#define EFI_DEVICE_ERROR 7
#define EFI_WRITE_PROTECTED 8
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NO_MEDIA 12
#define EFI_MEDIA_CHANGED 13
#define EFI_NOT_FOUND 14
#define EFI_TIMEOUT 18
#define EFI_SECURITY_VIOLATION 26
#define EFI_ERROR(s) ((s)!=0)
#define TPL_CALLBACK 8
#define SCSI_DISK_TIMEOUT 300000000ULL
#define ByProtocol 2
#define EDKII_UFS_HC_PLATFORM_COMMAND_POLICY_VERSION 4
#define EdkiiUfsHcGetMediaPolicy 8
#define EFI_SCSI_IO_DATA_DIRECTION_READ 0
#define EFI_SCSI_IO_STATUS_HOST_ADAPTER_OTHER 0x7f
#define EFI_SCSI_IO_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND 9
#define EFI_SCSI_IO_STATUS_HOST_ADAPTER_TIMEOUT 11
#define EFI_SCSI_IO_STATUS_HOST_ADAPTER_OK 0
#define EFI_SCSI_IO_STATUS_TARGET_GOOD 0
#define ZeroMem(p,n) memset(p,0,n)
typedef struct {BOOLEAN MediaPresent,ReadOnly;} EFI_BLOCK_IO_MEDIA;
typedef struct {EFI_BLOCK_IO_MEDIA *Media;} EFI_BLOCK_IO_PROTOCOL,EFI_BLOCK_IO2_PROTOCOL;
typedef struct {EFI_EVENT Event;EFI_STATUS TransactionStatus;} EFI_BLOCK_IO2_TOKEN;
typedef struct {unsigned Count;} LIST_ENTRY;
typedef struct {UINT64 Timeout;VOID *Cdb,*InDataBuffer,*OutDataBuffer,*SenseData;UINT32 InTransferLength,OutTransferLength;
 UINT8 CdbLength,DataDirection,SenseDataLength,HostAdapterStatus,TargetStatus;} EFI_SCSI_IO_SCSI_REQUEST_PACKET;
typedef struct IO IO;
struct IO {EFI_STATUS (*ExecuteScsiCommand)(IO*,EFI_SCSI_IO_SCSI_REQUEST_PACKET*,EFI_EVENT);};
typedef struct {EFI_HANDLE Handle;EFI_BLOCK_IO_PROTOCOL BlkIo;EFI_BLOCK_IO2_PROTOCOL BlkIo2;EFI_BLOCK_IO_MEDIA Media;
 VOID *EraseBlock,*StorageSecurity;IO *ScsiIo;BOOLEAN UfsPlatformSync,FixedDevice;UINT64 UfsSyncTimeout;LIST_ENTRY AsyncTaskQueue;} SCSI_DISK_DEV;
#define SCSI_DISK_DEV_FROM_BLKIO(p) ((SCSI_DISK_DEV*)((char*)(p)-offsetof(SCSI_DISK_DEV,BlkIo)))
#define SCSI_DISK_DEV_FROM_BLKIO2(p) ((SCSI_DISK_DEV*)((char*)(p)-offsetof(SCSI_DISK_DEV,BlkIo2)))
#define IS_DEVICE_FIXED(p) ((p)->FixedDevice)
typedef struct {BOOLEAN ReadOnly;UINT64 SyncTimeout;} EDKII_UFS_HC_MEDIA_POLICY;
typedef struct {unsigned Version;EFI_STATUS (*Callback)(EFI_HANDLE,unsigned,void*);} EDKII_UFS_HC_PLATFORM_PROTOCOL;
static EFI_GUID gEfiExtScsiPassThruProtocolGuid,gEdkiiUfsHcPlatformProtocolGuid,gEfiBlockIoProtocolGuid,gEfiBlockIo2ProtocolGuid,
 gEfiEraseBlockProtocolGuid,gEfiStorageSecurityCommandProtocolGuid;
static unsigned fault,executed,signals,policy_calls,freed,queue_checks;static UINT64 last_timeout;
static unsigned opcode,cdb_size,immed,payload,event_arg,policy_parent;
static BOOLEAN IsListEmpty(LIST_ENTRY *list){queue_checks++;return list->Count==0;}
static EFI_STATUS execute(IO *io,EFI_SCSI_IO_SCSI_REQUEST_PACKET *p,EFI_EVENT event){
 (void)io;executed++;opcode=((UINT8*)p->Cdb)[0];immed=((UINT8*)p->Cdb)[1];cdb_size=p->CdbLength;last_timeout=p->Timeout;
 payload=p->InTransferLength|p->OutTransferLength|(p->InDataBuffer!=NULL)|(p->OutDataBuffer!=NULL);event_arg=event!=NULL;
 if(fault==1)return EFI_TIMEOUT;
 p->HostAdapterStatus=fault==2?9:(fault==3?0x7e:0);p->TargetStatus=fault==4?2:0;
 if(fault==9){p->HostAdapterStatus=0x7f;p->TargetStatus=0xff;}
 return 0;
}
static EFI_STATUS policy(EFI_HANDLE parent,unsigned phase,void *data){
 policy_calls++;policy_parent=(uintptr_t)parent;
 if(parent!=(void*)2||phase!=EdkiiUfsHcGetMediaPolicy)return EFI_SECURITY_VIOLATION;
 if(fault==28)return EFI_UNSUPPORTED;
 EDKII_UFS_HC_MEDIA_POLICY *p=data;p->ReadOnly=true;p->SyncTimeout=fault==29?0:(fault==30?300000001:30000000);
 return 0;
}
static EDKII_UFS_HC_PLATFORM_PROTOCOL platform;
static EFI_STATUS locate(unsigned type,EFI_GUID *guid,void *key,UINTN *count,EFI_HANDLE **handles){
 (void)type;(void)guid;(void)key;static EFI_HANDLE h[3]={(void*)1,(void*)2,(void*)3};
 if(fault==34)return EFI_OUT_OF_RESOURCES;*count=3;*handles=h;return 0;
}
static EFI_STATUS EfiTestChildHandle(EFI_HANDLE parent,EFI_HANDLE child,EFI_GUID *guid){
 return parent==(void*)2&&child==(void*)9&&guid==&gEfiExtScsiPassThruProtocolGuid&&fault!=32?0:EFI_UNSUPPORTED;
}
static EFI_STATUS handle(EFI_HANDLE parent,EFI_GUID *guid,void **out){
 (void)guid;if(fault==33)return EFI_DEVICE_ERROR;if(fault==31)return EFI_UNSUPPORTED;
 if(parent!=(void*)2)return EFI_SECURITY_VIOLATION;*out=&platform;return 0;
}
static EFI_STATUS release(void *p){(void)p;freed++;return 0;}
static void FreePool(void *p){(void)p;freed++;}
static EFI_TPL raise(EFI_TPL p){return p;}static void restore(EFI_TPL p){(void)p;}
static EFI_STATUS signal(EFI_EVENT p){(void)p;signals++;return 0;}
static EFI_STATUS reinstall(EFI_HANDLE h,EFI_GUID *g,void *old,void *newp){(void)h;(void)g;(void)old;(void)newp;return 0;}
static struct {EFI_STATUS (*LocateHandleBuffer)(unsigned,EFI_GUID*,void*,UINTN*,EFI_HANDLE**);
 EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID*,void**);EFI_STATUS (*FreePool)(void*);
 EFI_TPL (*RaiseTPL)(EFI_TPL);void (*RestoreTPL)(EFI_TPL);EFI_STATUS (*SignalEvent)(EFI_EVENT);
 EFI_STATUS (*ReinstallProtocolInterface)(EFI_HANDLE,EFI_GUID*,void*,void*);} bs={locate,handle,release,raise,restore,signal,reinstall},*gBS=&bs;
static EFI_STATUS ScsiDiskDetectMedia(SCSI_DISK_DEV *d,BOOLEAN read,BOOLEAN *change){(void)d;(void)read;*change=false;return 0;}
static BOOLEAN DetermineInstallEraseBlock(SCSI_DISK_DEV *d,EFI_HANDLE h){(void)d;(void)h;return false;}
static BOOLEAN DetermineInstallStorageSecurity(SCSI_DISK_DEV *d,EFI_HANDLE h){(void)d;(void)h;return false;}
'''
        footer = r'''
void run(unsigned entry,unsigned scenario,UINT64 *out){
 unsigned token_mode=scenario/100;fault=scenario%100;executed=signals=policy_calls=freed=queue_checks=opcode=cdb_size=immed=payload=event_arg=policy_parent=0;last_timeout=0;
 IO io={execute};SCSI_DISK_DEV d={0};d.ScsiIo=&io;d.FixedDevice=true;d.Handle=(void*)9;
 d.BlkIo.Media=d.BlkIo2.Media=&d.Media;d.Media.MediaPresent=fault!=5;d.Media.ReadOnly=entry<10;
 d.UfsPlatformSync=entry<2;d.UfsSyncTimeout=fault==20?0:30000000;d.AsyncTaskQueue.Count=fault==6;
 platform=(EDKII_UFS_HC_PLATFORM_PROTOCOL){fault==26?3:4,fault==27?NULL:policy};
 EFI_BLOCK_IO2_TOKEN token={fault==8||token_mode==1?NULL:(void*)7,0xbeef};
 if(entry>=10){out[0]=ScsiDiskApplyUfsMediaPolicy(&d);if(!out[0]&&entry==11)out[0]=ScsiDiskFlushBlocks(&d.BlkIo);}
 else out[0]=entry==1||entry==3?ScsiDiskFlushBlocksEx(&d.BlkIo2,fault==7||token_mode==2?NULL:&token):ScsiDiskFlushBlocks(&d.BlkIo);
 out[1]=executed;out[2]=opcode;out[3]=cdb_size;out[4]=last_timeout;out[5]=payload;out[6]=event_arg;out[7]=immed;
 out[8]=token.TransactionStatus;out[9]=signals;out[10]=d.Media.ReadOnly;out[11]=policy_parent;out[12]=d.UfsPlatformSync;
 out[13]=freed;out[14]=queue_checks;out[15]=policy_calls;
}
'''
        (base/'test.c').write_text(header + '\n\n'.join(passthru.function(source, name) for name in names) + footer)
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(base/'test.c'), '-o', str(base/'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(base/'test.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def case(self, entry, fault=0):
        out = (ctypes.c_uint64 * 20)(); self.lib.run(entry, fault, out)
        return list(out)

    def test_real_synchronous_cdb_has_no_immediate_data_or_async_event(self):
        out = self.case(0)
        self.assertEqual(out[:8], [0, 1, 0x35, 10, 30000000, 0, 0, 0])
        self.assertEqual(out[10], 1)  # readonly policy does not turn sync into a no-op

    def test_command_timeout_unknown_host_status_and_check_condition_are_errors(self):
        for fault, expected in ((1, 18), (2, 18), (3, 7), (4, 7), (9, 7)):
            out = self.case(0, fault)
            self.assertEqual(out[:2], [expected, 1])  # never retried

    def test_no_media_outstanding_queue_and_invalid_deadline_never_submit(self):
        for fault, expected in ((5, 12), (6, 6), (20, 2)):
            self.assertEqual(self.case(0, fault)[:2], [expected, 0])

    def test_blockio2_completion_token_carries_actual_error_once(self):
        for fault, result in ((0, 0), (1, 18), (2, 18), (3, 7), (4, 7), (5, 12), (6, 6)):
            out = self.case(1, fault)
            self.assertEqual((out[0], out[8], out[9]), (0, result, 1))

    def test_blockio2_without_event_or_token_returns_blocking_error(self):
        self.assertEqual(self.case(1, 7)[9], 0)
        out = self.case(1, 8)
        self.assertEqual((out[0], out[8], out[9]), (0, 0, 0))
        self.assertEqual(self.case(1, 101)[0:2], [18, 1])
        self.assertEqual(self.case(1, 101)[8:10], [18, 0])
        self.assertEqual(self.case(1, 201)[0:2], [18, 1])
        self.assertEqual(self.case(1, 201)[9], 0)

    def test_actual_parent_policy_forces_readonly_without_mode_sense(self):
        out = self.case(11)
        self.assertEqual(out[:5], [0, 1, 0x35, 10, 30000000])
        self.assertEqual(out[10:14], [1, 2, 1, 1])

    def test_policy_failures_refuse_publication_instead_of_weakening_readonly(self):
        for fault, result in ((27, 26), (28, 3), (29, 2), (30, 2), (33, 7), (34, 9)):
            out = self.case(10, fault)
            self.assertEqual(out[0], result)
            self.assertEqual(out[12], 0)

    def test_foreign_and_legacy_controllers_do_not_receive_ufs_policy_or_new_sync(self):
        for fault in (26, 31, 32):
            out = self.case(11, fault)
            self.assertEqual(out[0:2], [0, 0])
            self.assertEqual(out[10:13], [0, 0, 0])
        self.assertEqual(self.case(2)[:2], [0, 0])
        self.assertEqual(self.case(3)[:2], [8, 0])
