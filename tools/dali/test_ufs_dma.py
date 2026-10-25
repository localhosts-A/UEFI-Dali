"""Execute the production DMA core and ARM Normal Non-cacheable adapter."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import platform
import tempfile
import unittest


class UfsDmaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler or sys.platform != 'linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('Linux MAP_32BIT and a C compiler required for UEFI address emulation')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base / 'Library').mkdir(); (base / 'Protocol').mkdir()
        (base / 'Uefi.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef bool BOOLEAN; typedef uint64_t EFI_STATUS;
typedef uint64_t EFI_PHYSICAL_ADDRESS; typedef struct { int dummy; } EFI_GUID;
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
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NOT_FOUND 14
#define EFI_ACCESS_DENIED 15
#define EFI_ALREADY_STARTED 20
#define EFI_ERROR(s) ((s)!=0)
#define MAX_UINTN SIZE_MAX
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_SIZE 4096
#define EFI_PAGES_TO_SIZE(p) ((p)*4096UL)
#define EFI_SIZE_TO_PAGES(n) (((n)+4095UL)/4096UL)
#define EFI_MEMORY_UC 1ULL
#define EFI_MEMORY_WC 2ULL
#define EFI_MEMORY_WT 4ULL
#define EFI_MEMORY_WB 8ULL
#define EFI_MEMORY_UCE 16ULL
#define EFI_MEMORY_WP 0x1000ULL
#define EFI_MEMORY_RP 0x2000ULL
#define EFI_MEMORY_XP 0x4000ULL
#define EFI_MEMORY_RO 0x20000ULL
#define AllocateMaxAddress 1
#define EfiBootServicesData 4
''')
        (base / 'Library/BaseMemoryLib.h').write_text('#include <string.h>\n#define ZeroMem(p,n) memset(p,0,n)\n#define CopyMem memcpy\n')
        (base / 'Library/BaseLib.h').write_text('#include <Uefi.h>\nvoid MemoryFence(void);\n')
        (base / 'Library/UefiBootServicesTableLib.h').write_text('''#include <Uefi.h>
typedef struct {
 EFI_STATUS (*AllocatePages)(int,int,UINTN,EFI_PHYSICAL_ADDRESS*);
 EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS,UINTN);
 EFI_STATUS (*LocateProtocol)(EFI_GUID*,void*,void**);
} EFI_BOOT_SERVICES;
extern EFI_BOOT_SERVICES *gBS;
''')
        (base / 'Library/DxeServicesTableLib.h').write_text('''#include <Uefi.h>
typedef struct { UINT64 BaseAddress,Length,Capabilities,Attributes; } EFI_GCD_MEMORY_SPACE_DESCRIPTOR;
typedef struct {
 EFI_STATUS (*GetMemorySpaceDescriptor)(EFI_PHYSICAL_ADDRESS,EFI_GCD_MEMORY_SPACE_DESCRIPTOR*);
 EFI_STATUS (*SetMemorySpaceAttributes)(EFI_PHYSICAL_ADDRESS,UINT64,UINT64);
} EFI_DXE_SERVICES;
extern EFI_DXE_SERVICES *gDS;
''')
        (base / 'Protocol/Cpu.h').write_text('''#include <Uefi.h>
typedef struct _CPU EFI_CPU_ARCH_PROTOCOL;
struct _CPU { EFI_STATUS (*FlushDataCache)(EFI_CPU_ARCH_PROTOCOL*,EFI_PHYSICAL_ADDRESS,UINT64,int); };
enum { EfiCpuFlushTypeWriteBackInvalidate, EfiCpuFlushTypeInvalidate };
extern EFI_GUID gEfiCpuArchProtocolGuid;
''')
        (base / 'harness.c').write_text('''#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <Library/Mt6991UfsDmaLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/Cpu.h>
typedef struct { void *cpu; size_t pages; uint64_t attr; } Allocation;
static Allocation allocated[32];
static unsigned scenario,live,allocs,frees,restores,ncsets,flushes,fences,restore_failed;
void MemoryFence(void) { ++fences; }
static EFI_STATUS allocate(int type,int memory,size_t pages,uint64_t *address) {
 if(type!=AllocateMaxAddress || memory!=EfiBootServicesData) return EFI_INVALID_PARAMETER;
 void *p=mmap(NULL,pages*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 if(p==MAP_FAILED) return EFI_OUT_OF_RESOURCES;
 if((uintptr_t)p+pages*4096-1>*address) { munmap(p,pages*4096); return EFI_OUT_OF_RESOURCES; }
 for(unsigned i=0;i<32;i++) if(!allocated[i].cpu) {
  allocated[i]=(Allocation){p,pages,EFI_MEMORY_WB|EFI_MEMORY_XP}; ++live;++allocs; *address=(uintptr_t)p; return 0;
 }
 munmap(p,pages*4096);return EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS release(uint64_t a,size_t pages) {
 ++frees;if(scenario==4 && frees==1) return EFI_DEVICE_ERROR;
 for(unsigned i=0;i<32;i++) if((uintptr_t)allocated[i].cpu==a && allocated[i].pages==pages) {
  if(allocated[i].attr!=(EFI_MEMORY_WB|EFI_MEMORY_XP)) return EFI_DEVICE_ERROR;
  munmap(allocated[i].cpu,pages*4096);allocated[i].cpu=NULL;--live;return 0;
 }\n return EFI_INVALID_PARAMETER;
}
static EFI_STATUS descriptor(uint64_t a,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *d) {
 for(unsigned i=0;i<32;i++) if((uintptr_t)allocated[i].cpu==a) {
  *d=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){a,allocated[i].pages*4096,EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WB|EFI_MEMORY_XP,allocated[i].attr};
  if(scenario==15)d->Capabilities&=~EFI_MEMORY_WC;
  return 0;
 }\n return EFI_NOT_FOUND;
}
static EFI_STATUS attributes(uint64_t a,uint64_t len,uint64_t attrs) {
 for(unsigned i=0;i<32;i++) if((uintptr_t)allocated[i].cpu==a && len==allocated[i].pages*4096) {
  if(attrs&EFI_MEMORY_UC)return EFI_UNSUPPORTED;
  if(attrs&EFI_MEMORY_WC) {++ncsets;allocated[i].attr=attrs;if(scenario==1)return EFI_DEVICE_ERROR;}
  else {++restores;if(scenario==2&&!restore_failed++){return EFI_DEVICE_ERROR;}allocated[i].attr=attrs;}
  return 0;
 }\n return EFI_INVALID_PARAMETER;
}
static EFI_STATUS flush(EFI_CPU_ARCH_PROTOCOL *cpu,uint64_t a,uint64_t len,int type) {
 (void)cpu;(void)a;(void)len;++flushes;
 return scenario==3 && type==EfiCpuFlushTypeInvalidate ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_CPU_ARCH_PROTOCOL cpu={flush}; EFI_GUID gEfiCpuArchProtocolGuid;
static EFI_STATUS locate(EFI_GUID *g,void *r,void **out) {(void)g;(void)r;*out=&cpu;return 0;}
static EFI_BOOT_SERVICES bs={allocate,release,locate};EFI_BOOT_SERVICES *gBS=&bs;
static EFI_DXE_SERVICES ds={descriptor,attributes};EFI_DXE_SERVICES *gDS=&ds;
int run(unsigned which,uint64_t *out) {
 scenario=which;live=allocs=frees=restores=ncsets=flushes=fences=restore_failed=0;
 memset(allocated,0,sizeof allocated);
 MT6991_UFS_DMA d={0};void *host=NULL,*uc=NULL;uint64_t dev=0,id=0,old=0;
 unsigned char data[66];memset(data,0x55,sizeof data);
 out[0]=Mt6991UfsDmaUefiInit(&d,0xffffffff);
 if(which<=4 || which==15) {
  out[1]=Mt6991UfsDmaAllocate(&d,1,&host);
  out[2]=live;
  if(host)out[3]=Mt6991UfsDmaFree(&d,1,host);
  out[4]=live;out[5]=d.Poisoned;
  out[6]=Mt6991UfsDmaReleaseAll(&d);
  out[7]=Mt6991UfsDmaStopped(&d,1);out[8]=live;
  out[9]=Mt6991UfsDmaStopped(&d,0);out[10]=Mt6991UfsDmaReleaseAll(&d);
 } else if(which==5) {
  out[1]=Mt6991UfsDmaAllocate(&d,1,&host);
  out[2]=Mt6991UfsDmaMap(&d,Mt6991DmaCommon,data,32,&dev,&id);
  out[3]=Mt6991UfsDmaMap(&d,Mt6991DmaCommon,host,4096,&dev,&id);
  out[4]=Mt6991UfsDmaFree(&d,1,host);
  out[5]=Mt6991UfsDmaResolve(&d,dev+4095,2,true,&uc);
  out[6]=Mt6991UfsDmaUnmap(&d,id);out[7]=Mt6991UfsDmaFree(&d,1,host);
 } else if(which>=6 && which<=10) {
  out[1]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceWrite,data+1,64,&dev,&id);old=id;
  out[2]=Mt6991UfsDmaResolve(&d,dev,64,false,&uc);
  for(unsigned i=0;i<64;i++)((volatile unsigned char*)uc)[i]=0xa0;
  out[3]=Mt6991UfsDmaSubmit(&d);out[4]=Mt6991UfsDmaUnmap(&d,id);
  if(which==7) {
   Mt6991UfsDmaPoison(&d);out[5]=Mt6991UfsDmaComplete(&d,true);
   out[6]=Mt6991UfsDmaStopped(&d,1);out[7]=Mt6991UfsDmaReleaseAll(&d);out[8]=live;
   Mt6991UfsDmaStopped(&d,0);out[9]=Mt6991UfsDmaReleaseAll(&d);
  } else {
   out[5]=Mt6991UfsDmaComplete(&d,which!=8);
   out[6]=Mt6991UfsDmaSubmit(&d); // old completed data mapping cannot be resubmitted
   out[7]=Mt6991UfsDmaUnmap(&d,id);
   out[8]=Mt6991UfsDmaUnmap(&d,id);
   if(which==9) {
    Mt6991UfsDmaMap(&d,Mt6991DmaDeviceWrite,data+1,64,&dev,&id);
    out[9]=id!=old;out[10]=Mt6991UfsDmaUnmap(&d,old);Mt6991UfsDmaUnmap(&d,id);
   }
  }
  out[11]=data[1];out[12]=data[0];out[13]=data[65];
 } else if(which==11) {
  out[1]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data+1,64,&dev,&id);
  Mt6991UfsDmaResolve(&d,dev,64,false,&uc);out[2]=((unsigned char*)uc)[0];
  out[3]=Mt6991UfsDmaResolve(&d,UINT64_MAX-1,4,false,&uc);
  out[4]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,(void*)(UINTPTR_MAX-1),4,&dev,&old);
  Mt6991UfsDmaUnmap(&d,id);
 } else if(which==12) {
  uint64_t ids[16];for(unsigned i=0;i<16;i++)out[1]|=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data,1,&dev,&ids[i]);
  out[2]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data,1,&dev,&id);out[3]=live;
  for(unsigned i=0;i<16;i++)Mt6991UfsDmaUnmap(&d,ids[i]);
 } else if(which==13) {
  out[1]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data,0,&dev,&id);
  out[2]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data,0x100001,&dev,&id);
  out[3]=Mt6991UfsDmaMap(&d,(MT6991_DMA_OPERATION)-1,data,1,&dev,&id);
  out[4]=Mt6991UfsDmaAllocate(&d,0,&host);
  out[5]=Mt6991UfsDmaAllocate(&d,SIZE_MAX,&host);
  d.NextId=UINT64_MAX;out[6]=Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data,1,&dev,&id);
 } else if(which==14) {
  Mt6991UfsDmaAllocate(&d,1,&host);
  Mt6991UfsDmaMap(&d,Mt6991DmaCommon,host,4096,&dev,&old);
  Mt6991UfsDmaMap(&d,Mt6991DmaDeviceRead,data,1,&dev,&id);
  out[1]=Mt6991UfsDmaStopped(&d,1);
  out[2]=Mt6991UfsDmaFree(&d,1,host);
  out[3]=Mt6991UfsDmaUnmap(&d,old);out[4]=Mt6991UfsDmaUnmap(&d,id);
  out[5]=live;out[6]=Mt6991UfsDmaStopped(&d,0);out[7]=Mt6991UfsDmaReleaseAll(&d);
 }
 out[20]=live;out[21]=allocs;out[22]=frees;out[23]=restores;out[24]=ncsets;out[25]=flushes;
 out[26]=fences;out[27]=d.Busy;
 for(unsigned i=0;i<32;i++)if(allocated[i].cpu){munmap(allocated[i].cpu,allocated[i].pages*4096);allocated[i].cpu=NULL;}
 return 0;
}
''')
        pkg = Path(__file__).resolve().parents[2] / 'Silicon/MediaTek/MT6991Pkg'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(pkg / 'Include'),
                        str(pkg / 'Library/UfsDmaLib/UfsDma.c'), str(pkg / 'Library/UfsDmaLib/UfsDmaUefi.c'),
                        str(base / 'harness.c'), '-o', str(base / 'dma.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'dma.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def case(self, which):
        out = (ctypes.c_uint64 * 32)(); self.lib.run(which, out)
        self.assertEqual(out[0], 0)
        return list(out)

    def test_real_uefi_backend_uses_normal_noncacheable_and_restores_before_free(self):
        out = self.case(0)
        self.assertEqual(out[1:6], [0, 1, 0, 0, 0])
        self.assertEqual(out[20:26], [0, 1, 1, 1, 1, 2])

    def test_failed_attribute_update_and_post_change_cache_flush_are_rolled_back(self):
        for scenario in (1, 3):
            out = self.case(scenario)
            self.assertEqual((out[1], out[2], out[20], out[23]), (7, 0, 0, 1))

    def test_failed_restore_retains_record_until_confirmed_stop_and_retry(self):
        out = self.case(2)
        self.assertEqual(out[3:11], [7, 1, 1, 6, 6, 1, 0, 0])
        self.assertEqual((out[20], out[22], out[23]), (0, 1, 2))

    def test_failed_free_does_not_repeat_successful_attribute_restore(self):
        out = self.case(4)
        self.assertEqual((out[3], out[4], out[20], out[22], out[23]), (7, 1, 0, 2, 1))

    def test_common_buffer_requires_owned_full_span_and_unmapped_allocation(self):
        out = self.case(5)
        self.assertEqual(out[1:8], [0, 3, 0, 15, 14, 0, 0])
        self.assertEqual(out[20], 0)

    def test_device_write_copyback_requires_completion_and_preserves_guards(self):
        out = self.case(6)
        self.assertEqual(out[1:9], [0, 0, 0, 6, 0, 6, 0, 2])
        self.assertEqual(out[11:14], [0xa0, 0x55, 0x55])
        self.assertEqual(out[20], 0)

    def test_uncertain_request_cannot_free_until_hce_zero_and_never_copies_back(self):
        out = self.case(7)
        self.assertEqual(out[4:10], [6, 2, 6, 6, 1, 0])
        self.assertEqual((out[11], out[20]), (0x55, 0))

    def test_failed_completion_and_unsubmitted_cleanup_leave_user_data_untouched(self):
        self.assertEqual(self.case(8)[11:14], [0x55] * 3)

    def test_mapping_ids_are_not_reused_and_stale_unmap_is_rejected(self):
        out = self.case(9)
        self.assertEqual(out[9:11], [1, 2])

    def test_transmit_bounces_and_all_span_checks_reject_overflow(self):
        out = self.case(11)
        self.assertEqual(out[1:5], [0, 0x55, 2, 2])
        self.assertEqual(out[20], 0)

    def test_mapping_capacity_is_bounded_and_leaves_no_untracked_pages(self):
        out = self.case(12)
        self.assertEqual(out[1:4], [0, 9, 16])
        self.assertEqual(out[20], 0)

    def test_zero_oversize_invalid_operation_and_handle_wrap_never_allocate(self):
        out = self.case(13)
        self.assertEqual(out[1:7], [2, 2, 2, 4, 4, 9])
        self.assertEqual(out[20:22], [0, 0])

    def test_all_individual_release_paths_require_confirmed_stop_after_poison(self):
        out = self.case(14)
        self.assertEqual(out[1:8], [6, 6, 6, 6, 2, 0, 0])
        self.assertEqual(out[20], 0)

    def test_uc_only_gcd_does_not_fall_back_to_device_memory(self):
        out = self.case(15)
        self.assertEqual((out[1], out[20], out[24], out[25]), (3, 0, 0, 0))
