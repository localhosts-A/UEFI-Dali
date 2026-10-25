"""Execute the production BlockIo diagnostic with media and transport faults."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler: raise unittest.SkipTest('C compiler required')
        cls.temp = tempfile.TemporaryDirectory(); base = Path(cls.temp.name)
        for folder in ('Library', 'Protocol'): (base/folder).mkdir()
        (base/'Uefi.h').write_text(r'''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef size_t UINTN;typedef char CHAR8;typedef uint16_t CHAR16;typedef bool BOOLEAN;typedef uint64_t EFI_STATUS,EFI_LBA;
typedef void *EFI_HANDLE;typedef int EFI_GUID;
#define VOID void
#define CONST const
#define STATIC static
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_DEVICE_ERROR 7
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NO_MEDIA 12
#define EFI_MEDIA_CHANGED 13
#define EFI_NOT_FOUND 14
#define EFI_ACCESS_DENIED 15
#define EFI_TIMEOUT 18
#define EFI_ERROR(s) ((s)!=0)
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_SIZE 4096
#define EFI_SIZE_TO_PAGES(n) (((n)+4095)/4096)
''')
        (base/'Library/BaseMemoryLib.h').write_text('#include <string.h>\n#define CompareMem memcmp\n#define CopyMem memcpy\n#define ZeroMem(p,n) memset(p,0,n)\n')
        (base/'Library/BaseCryptLib.h').write_text('#include <Uefi.h>\n#define SHA256_DIGEST_SIZE 32\nBOOLEAN Sha256HashAll(CONST VOID*,UINTN,UINT8*);\n')
        (base/'Library/DebugLib.h').write_text('#define DEBUG_WARN 1\nvoid debug(int,const char*,...);\n#define DEBUG(args) debug args\n')
        (base/'Library/UefiLib.h').write_text('#include <Uefi.h>\nUINTN Print(CONST CHAR16*,...);\n')
        (base/'Library/MemoryAllocationLib.h').write_text('#include <Uefi.h>\nVOID *AllocateAlignedPages(UINTN,UINTN);\nVOID FreeAlignedPages(VOID*,UINTN);\n')
        (base/'Library/DevicePathLib.h').write_text(r'''#pragma once
#include <Uefi.h>
typedef struct {UINT8 Type,SubType,Length[2];} EFI_DEVICE_PATH_PROTOCOL;
typedef struct {EFI_DEVICE_PATH_PROTOCOL Header;UINT8 Guid[16];} VENDOR_DEVICE_PATH;
typedef struct {EFI_DEVICE_PATH_PROTOCOL Header;UINT8 Pun,Lun;} UFS_DEVICE_PATH;
#pragma pack(push,1)
typedef struct {EFI_DEVICE_PATH_PROTOCOL Header;UINT32 PartitionNumber;UINT64 PartitionStart,PartitionSize;UINT8 Signature[16],MBRType,SignatureType;} HARDDRIVE_DEVICE_PATH;
#pragma pack(pop)
#define MESSAGING_DEVICE_PATH 3
#define MSG_UFS_DP 0x19
#define MEDIA_DEVICE_PATH 4
#define MEDIA_HARDDRIVE_DP 1
#define SIGNATURE_TYPE_GUID 2
#define DevicePathNodeLength(p) ((p)->Length[0]+((p)->Length[1]<<8))
#define IsDevicePathEnd(p) ((p)->Type==0x7f)
#define DevicePathType(p) ((p)->Type)
#define DevicePathSubType(p) ((p)->SubType)
UINTN GetDevicePathSize(CONST VOID*);
''')
        (base/'Protocol/BlockIo.h').write_text(r'''#pragma once
#include <Uefi.h>
typedef struct {UINT32 MediaId,BlockSize,IoAlign;UINT64 LastBlock;BOOLEAN ReadOnly,MediaPresent,LogicalPartition;} EFI_BLOCK_IO_MEDIA;
typedef struct IO IO;struct IO {EFI_BLOCK_IO_MEDIA *Media;EFI_STATUS (*ReadBlocks)(IO*,UINT32,EFI_LBA,UINTN,VOID*);EFI_STATUS (*FlushBlocks)(IO*);};
typedef IO EFI_BLOCK_IO_PROTOCOL;
''')
        (base/'Library/UefiBootServicesTableLib.h').write_text(r'''#pragma once
#include <Uefi.h>
#define ByProtocol 2
extern EFI_GUID gEfiDevicePathProtocolGuid,gEfiBlockIoProtocolGuid;
typedef struct {EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID*,VOID**);
 EFI_STATUS (*LocateHandleBuffer)(unsigned,EFI_GUID*,VOID*,UINTN*,EFI_HANDLE**);EFI_STATUS (*FreePool)(VOID*);} BS;
extern BS *gBS;
''')
        (base/'harness.c').write_text(r'''
#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include "UfsHostInternal.h"
static UINT8 parent[24],paths[4][80];static IO devices[4];static EFI_BLOCK_IO_MEDIA media[4];
static unsigned fault,reads,hashes,syncs,allocations,frees,freed_pages,token_errors,screen_prints;
static volatile UINT64 completed[8];
static UINT64 lbas[16],sizes[16];static const char *run_token="0123456789abcdef0123456789abcdef";
EFI_GUID gEfiDevicePathProtocolGuid=1,gEfiBlockIoProtocolGuid=2;
UINTN GetDevicePathSize(CONST VOID *p){return p==parent?24:(p==paths[2]?72:30);}
void debug(int level,const char *format,...){(void)level;(void)format;}
UINTN Print(CONST CHAR16 *format,...){(void)format;screen_prints++;return 0;}
VOID *AllocateAlignedPages(UINTN pages,UINTN alignment){void *p=NULL;allocations++;return posix_memalign(&p,alignment,pages*4096)?NULL:p;}
VOID FreeAlignedPages(VOID *p,UINTN pages){frees++;freed_pages+=pages;free(p);}
BOOLEAN Sha256HashAll(CONST VOID *data,UINTN bytes,UINT8 *digest){
 (void)data;(void)bytes;hashes++;memset(digest,0xa5,32);return fault!=6;
}
static EFI_STATUS read_blocks(IO *io,UINT32 id,EFI_LBA lba,UINTN bytes,VOID *buffer){
 if(id!=io->Media->MediaId||bytes!=io->Media->BlockSize||lba>io->Media->LastBlock)return EFI_INVALID_PARAMETER;
 lbas[reads]=lba;sizes[reads]=bytes;reads++;if(fault==4&&reads==2)return EFI_DEVICE_ERROR;
 memset(buffer,0x5a,bytes);if(fault==7)io->Media->BlockSize=65536;return 0;
}
static EFI_STATUS flush(IO *io){syncs++;if(fault==5)return EFI_TIMEOUT;if(fault!=13)completed[io==&devices[1]?0:2]++;
 if(fault==14)io->Media->MediaId++;return 0;}
static EFI_STATUS handle(EFI_HANDLE h,EFI_GUID *g,VOID **out){
 if(h==(void*)99&&g==&gEfiDevicePathProtocolGuid){*out=parent;return 0;}
 unsigned i=(uintptr_t)h-1;if(i>=4)return EFI_NOT_FOUND;
 if(g==&gEfiDevicePathProtocolGuid){*out=paths[i];return 0;}
 if(g==&gEfiBlockIoProtocolGuid){*out=&devices[i];return 0;}return EFI_UNSUPPORTED;
}
static EFI_STATUS locate(unsigned type,EFI_GUID *g,VOID *key,UINTN *count,EFI_HANDLE **out){
 (void)type;(void)g;(void)key;static EFI_HANDLE handles[4]={(void*)1,(void*)2,(void*)3,(void*)4};
 *count=fault==10?2:4;*out=handles+(fault==10?2:0);return 0;
}
static EFI_STATUS release(VOID *p){(void)p;return 0;}
static BS services={handle,locate,release};BS *gBS=&services;
void run(unsigned which,UINT64 *out){
 fault=which;reads=hashes=syncs=allocations=frees=freed_pages=token_errors=screen_prints=0;memset(lbas,0,sizeof lbas);memset(sizes,0,sizeof sizes);
 for(unsigned i=0;i<8;i++)completed[i]=0;
 memset(parent,0,sizeof parent);parent[0]=1;parent[1]=4;parent[2]=20;parent[4]=0xa5;parent[20]=0x7f;parent[21]=0xff;parent[22]=4;
 for(unsigned i=0;i<4;i++){
  memset(paths[i],0,sizeof paths[i]);memcpy(paths[i],parent,20);
  paths[i][20]=3;paths[i][21]=0x19;paths[i][22]=6;paths[i][25]=i==1?0:2;
  paths[i][26]=0x7f;paths[i][27]=0xff;paths[i][28]=4;
  media[i]=(EFI_BLOCK_IO_MEDIA){123+i,i==1?512:4096,8,i==1?7:64577535,true,true,i==2};
  devices[i]=(IO){&media[i],read_blocks,flush};
 }
 HARDDRIVE_DEVICE_PATH hd={{4,1,{42,0}},71,418560,16384,{1},2,2};
 memcpy(paths[2]+26,&hd,sizeof hd);paths[2][68]=0x7f;paths[2][69]=0xff;paths[2][70]=4;media[2].LastBlock=16383;
 paths[3][4]=0xff;
 if(which==1)media[0].ReadOnly=false;
 if(which==2)media[2].ReadOnly=false;
 if(which==3)media[0].BlockSize=512;
 if(which==8)paths[1][25]=2;
 if(which==9)paths[0][22]=3;
 if(which==11)media[0].IoAlign=3;
 if(which==12)media[1].BlockSize=65536;
 out[0]=DaliUfsReadOnlyDiagnostics((void*)99,run_token,completed);out[1]=reads;out[2]=hashes;out[3]=syncs;
 out[4]=allocations;out[5]=frees;out[6]=freed_pages;
 for(unsigned i=0;i<8;i++){out[8+i]=lbas[i];out[16+i]=sizes[i];}
 out[24]=screen_prints;
}
''')
        pkg = Path(__file__).resolve().parents[2] / 'Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(pkg), str(pkg/'UfsDiagnostics.c'), str(base/'harness.c'),
                        '-o', str(base/'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(base/'test.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def case(self, fault=0):
        out = (ctypes.c_uint64 * 32)(); self.lib.run(fault, out)
        return list(out)

    def test_actual_blockio_lbas_geometry_and_device_sync_are_used(self):
        out = self.case()
        self.assertEqual(out[:7], [0, 7, 7, 2, 2, 2, 2])
        self.assertEqual(out[8:15], [0, 1, 64577535, 418560, 0, 1, 7])
        self.assertEqual(out[16:23], [4096]*4+[512]*3)

    def test_writable_whole_disk_or_partition_is_a_failure(self):
        self.assertEqual(self.case(1)[:2], [15, 0])
        self.assertEqual(self.case(2)[0], 15)

    def test_known_boot_sample_requires_observed_geometry_not_fixed_block_assumption(self):
        out = self.case(3)
        self.assertEqual(out[:4], [14, 6, 6, 2])
        self.assertEqual(out[16:22], [512]*6)
        self.assertEqual(self.case(12)[16:23], [4096]*4+[65536]*3)

    def test_read_error_and_sync_failure_are_not_retried(self):
        self.assertEqual(self.case(4)[:6], [7, 2, 1, 0, 1, 1])
        self.assertEqual(self.case(5)[:6], [18, 4, 4, 1, 1, 1])
        self.assertEqual(self.case(4)[24], 2)  # the failing read sample is visible on-screen
        self.assertEqual(self.case(5)[24], 3)  # the failing sync stage is visible on-screen

    def test_hash_failure_and_media_change_never_hash_stale_or_wrong_sized_data(self):
        self.assertEqual(self.case(6)[:7], [7, 1, 1, 0, 1, 1, 1])
        self.assertEqual(self.case(7)[:7], [13, 1, 0, 0, 1, 1, 1])

    def test_duplicate_lun_malformed_path_or_alignment_refuses_qualification(self):
        self.assertEqual(self.case(8)[0], 7)
        self.assertEqual(self.case(9)[:2], [7, 0])
        self.assertEqual(self.case(11)[:2], [3, 0])
        self.assertEqual(self.case(10)[:2], [14, 0])

    def test_successful_flush_without_hardware_command_completion_is_not_sync(self):
        self.assertEqual(self.case(13)[:6], [7, 4, 4, 1, 1, 1])

    def test_media_change_during_successful_sync_refuses_qualification(self):
        self.assertEqual(self.case(14)[:7], [13, 4, 4, 1, 1, 1, 1])
