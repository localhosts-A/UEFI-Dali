"""Run the production wire guard with real DMA ownership and hostile descriptors."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import unittest

import test_ufs_dma as dma_test
from mu_patch_sources import patched_mu_source
from test_ufs_passthru_policy import function


class UfsHostPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dma_test.UfsDmaTests.setUpClass()
        base = Path(dma_test.UfsDmaTests.temp.name)
        with (base / 'Uefi.h').open('a') as f:
            f.write('\ntypedef uint16_t UINT16; typedef char CHAR8;\n#define MAX_UINT16 UINT16_MAX\n#define MAX_UINT32 UINT32_MAX\n#define EFI_WRITE_PROTECTED 8\n')
        with (base / 'Library/BaseMemoryLib.h').open('a') as f:
            f.write('#define CompareMem memcmp\n')
        root = Path(__file__).resolve().parents[2]
        with patched_mu_source(root) as mu:
            upiu = (mu/'MdePkg/Include/IndustryStandard/Ufs.h').read_text()
            hci = (mu/'MdePkg/Include/IndustryStandard/UfsHci.h').read_text()
            source = (mu/'MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruHci.c').read_text()
        command_type = re.search(r'typedef struct \{.*?\} UTP_COMMAND_UPIU;',upiu,re.S).group()
        direction_type = re.search(r'typedef enum \{\s*UfsNoData.*?\} UFS_DATA_DIRECTION;',hci,re.S).group()
        (base/'mu-upiu.h').write_text(r'''
#include <assert.h>
#include <Library/BaseMemoryLib.h>
#define ASSERT assert
#define IN
#define OUT
#define BIT0 1
#define BIT5 32
#define BIT6 64
#pragma pack(push,1)
'''+command_type+'\n#pragma pack(pop)\n'+direction_type+r'''
_Static_assert(sizeof(UTP_COMMAND_UPIU)==32,"actual Mu command UPIU size");
static void SwapLittleEndianToBigEndian(UINT8 *p,UINTN bytes){
 for(UINTN i=0;i<bytes/2;i++){UINT8 t=p[i];p[i]=p[bytes-1-i];p[bytes-1-i]=t;}
}
'''+function(source,'UfsInitCommandUpiu'))
        (base / 'policy.c').write_text(r'''
#include "harness.c"
#include <Library/Mt6991UfsPolicy.h>
#include "mu-upiu.h"
static void le16(unsigned char *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void le32(unsigned char *p,unsigned n){le16(p,n);le16(p+2,n>>16);}
static void be16(unsigned char *p,unsigned n){p[0]=n>>8;p[1]=n;}
static void be32(unsigned char *p,unsigned n){be16(p,n>>16);be16(p+2,n);}
static void be64(unsigned char *p,uint64_t n){be32(p,n>>32);be32(p+4,n);}
uint64_t command(unsigned op,unsigned lun,unsigned cbytes,uint64_t lba,uint64_t count,
                 unsigned block,uint64_t blocks,unsigned bytes,unsigned ready,unsigned immediate) {
 MT6991_UFS_GEOMETRY g[8]={0};unsigned char cdb[16]={0};
 if(lun<8)g[lun]=(MT6991_UFS_GEOMETRY){blocks,block,ready!=0,FALSE,FALSE};
 cdb[0]=op;cdb[1]=immediate?2:0;
 if(op==0x28){be32(cdb+2,lba);be16(cdb+7,count);}
 if(op==0x88){be64(cdb+2,lba);be32(cdb+10,count);}
 if(op==0x9e){cdb[1]=0x10;be32(cdb+10,bytes);}
 return Mt6991UfsReadOnlyCommand(g,lun,cdb,cbytes,bytes);
}
uint64_t scratch(unsigned mode) {
 MT6991_UFS_GEOMETRY g[8]={0};MT6991_UFS_SCRATCH_GRANT grant={0};unsigned char cdb[16]={0};
 g[2]=(MT6991_UFS_GEOMETRY){64577536,4096,true,true,false};
 grant.Enabled=true;grant.Lun=2;strcpy(grant.Token,"0123456789abcdef0123456789abcdef");
 memset(grant.DeviceBinding,1,sizeof grant.DeviceBinding);memset(grant.CandidateSha256,2,sizeof grant.CandidateSha256);
 memset(grant.SourceSha256,3,sizeof grant.SourceSha256);memset(grant.PartitionGuid,4,sizeof grant.PartitionGuid);
 grant.BlockSize=4096;grant.CapacityBlocks=64577536;grant.BootStartLba=418560;grant.BootBlocks=16384;
 grant.CandidateBytes=5062656;grant.ScratchStartLba=434432;grant.ScratchBlocks=256;grant.TailGuardBlocks=256;
 unsigned lun=2,cdbBytes=10,bytes=4096;uint64_t lba=434432,count=1;cdb[0]=0x2a;
 if(mode>=30){
  grant.Target=MT6991_UFS_TARGET_USERDATA_LOGS;grant.BootStartLba=64565240;grant.BootBlocks=8192;
  grant.CandidateBytes=0;grant.Sequence=1;grant.ScratchStartLba=64565240;
  grant.ScratchBlocks=1024;grant.TailGuardBlocks=7168;lba=64565240;
  const UINT8 logsGuid[16]={0xcd,0xe9,0xab,0x32,0x18,0xd8,0x47,0x77,0x8c,0xed,0x69,0x1e,0x7c,0xd5,0xc6,0x36};
  memcpy(grant.PartitionGuid,logsGuid,16);
  if(mode==31)lba=64565240+1023;if(mode==32)lba=64565240+1024;
  if(mode==33){lba=64565240+1023;count=2;bytes=8192;}
  if(mode==34)grant.Sequence=2;
  if(mode==35)grant.ScratchStartLba++;
  if(mode==36)grant.TailGuardBlocks--;
 }
 switch(mode){
 case 1:cdb[0]=0x8a;cdbBytes=16;lba=434687;break;
 case 2:lba=434687;count=2;bytes=8192;break;
 case 3:lba=434431;break;
 case 4:lba=434688;break;
 case 5:count=0;bytes=0;break;
 case 6:g[2].Blocks-- ;break;
 case 7:g[2].Valid=false;break;
 case 8:g[2].WriteProtectionKnown=false;break;
 case 9:g[2].WriteProtected=true;break;
 case 10:lun=1;break;
 case 11:grant.Enabled=false;break;
 case 12:grant.Token[0]='z';break;
 case 13:memset(grant.CandidateSha256,0,sizeof grant.CandidateSha256);break;
 case 14:memset(grant.SourceSha256,0,sizeof grant.SourceSha256);break;
 case 15:memset(grant.DeviceBinding,0,sizeof grant.DeviceBinding);break;
 case 16:memset(grant.PartitionGuid,0,sizeof grant.PartitionGuid);break;
 case 17:grant.CandidateBytes=64ULL*1024*1024;break;
 case 18:grant.ScratchStartLba++;break;
 case 19:grant.TailGuardBlocks--;break;
 case 20:cdbBytes=6;break;
 case 21:cdb[1]=8;break;
 case 22:cdb[6]=1;break;
 case 23:cdb[9]=1;break;
 case 24:cdb[0]=0x8a;cdbBytes=16;cdb[1]=8;break;
 case 25:cdb[0]=0x8a;cdbBytes=16;cdb[14]=1;break;
 case 26:cdb[0]=0x8a;cdbBytes=16;cdb[15]=1;break;
 }
 if(cdb[0]==0x2a){be32(cdb+2,lba);be16(cdb+7,count);}else{be64(cdb+2,lba);be32(cdb+10,count);}
 return Mt6991UfsScratchWriteCommand(g,&grant,lun,cdb,cdbBytes,bytes);
}
static void grant_init(MT6991_UFS_SCRATCH_GRANT *grant);
uint64_t scratch_map(unsigned mode,uint64_t *absolute,uint64_t *count) {
 MT6991_UFS_GEOMETRY g[8]={0};MT6991_UFS_SCRATCH_GRANT grant={0};
 grant_init(&grant);g[2]=(MT6991_UFS_GEOMETRY){64577536,4096,true,true,false};
 uint64_t lba=0;unsigned bytes=4096;bool write=false;
 switch(mode){
 case 1:lba=255;break;                 /* final block in the one-MiB window */
 case 2:lba=255;bytes=8192;break;      /* range spills into the tail guard */
 case 3:lba=256;break;                 /* first block after the grant */
 case 4:bytes=4095;break;              /* unaligned transfer */
 case 5:g[2].BlockSize=512;break;      /* runtime geometry mismatch */
 case 6:g[2].WriteProtectionKnown=false;write=true;break;
 case 7:g[2].WriteProtected=true;write=true;break;
 case 8:write=true;break;
 case 9:lba=UINT64_MAX;break;
 case 10:g[2].Blocks--;break;
 }
 return Mt6991UfsScratchMapRange(g,&grant,2,lba,bytes,write,absolute,count);
}
uint64_t build_rw(unsigned mode,unsigned char *cdb,uint64_t *bytes) {
 uint64_t lba=434432,count=1;bool write=false;
 switch(mode){case 1:lba=UINT64_C(0x100000000);break;case 2:write=true;break;
 case 3:lba=UINT64_C(0x100000000);write=true;break;case 4:count=0;break;
 case 5:count=UINT64_C(0x100000000);break;case 6:lba=UINT64_MAX;count=2;break;}
 size_t length=0;uint64_t status=Mt6991UfsBuildReadWriteCdb(write,lba,count,cdb,&length);*bytes=length;return status;
}
static void grant_init(MT6991_UFS_SCRATCH_GRANT *grant) {
 memset(grant,0,sizeof *grant);grant->Enabled=true;grant->Lun=2;
 strcpy(grant->Token,"0123456789abcdef0123456789abcdef");
 memset(grant->DeviceBinding,1,sizeof grant->DeviceBinding);memset(grant->CandidateSha256,2,sizeof grant->CandidateSha256);
 memset(grant->SourceSha256,3,sizeof grant->SourceSha256);memset(grant->PartitionGuid,4,sizeof grant->PartitionGuid);
 grant->BlockSize=4096;grant->CapacityBlocks=64577536;grant->BootStartLba=418560;grant->BootBlocks=16384;
 grant->CandidateBytes=5062656;grant->ScratchStartLba=434432;grant->ScratchBlocks=256;grant->TailGuardBlocks=256;
}
void wire(unsigned which,uint64_t *out) {
 scenario=0;live=allocs=frees=restores=ncsets=flushes=fences=restore_failed=0;
 memset(allocated,0,sizeof allocated);
 MT6991_UFS_DMA d={0};MT6991_UFS_GEOMETRY g[8]={0};MT6991_UFS_SCRATCH_GRANT grant={0};MT6991_UFS_PENDING p;
 unsigned char *trd=NULL,*q=NULL,*r=NULL,host[4096]={0};void *cpu;
 uint64_t trl,cmd,id1,id2,data=0,id3=0,extra;
 Mt6991UfsDmaUefiInit(&d,0xffffffff);
 Mt6991UfsDmaAllocate(&d,1,(void**)&trd);Mt6991UfsDmaAllocate(&d,1,(void**)&q);
 Mt6991UfsDmaMap(&d,Mt6991DmaCommon,trd,4096,&trl,&id1);
 Mt6991UfsDmaMap(&d,Mt6991DmaCommon,q,4096,&cmd,&id2);
 le32(trd,0x15000000);le32(trd+8,15);le32(trd+16,cmd);le16(trd+24,14);le16(trd+26,8);
 le16(trd+28,1);le16(trd+30,22);q[0]=1;q[1]=0x41;q[2]=2;q[3]=0x72;be32(q+12,8);q[16]=0x25;
 if(which<20) {
  Mt6991UfsDmaMap(&d,which==6?Mt6991DmaDeviceRead:Mt6991DmaDeviceWrite,host,8,&data,&id3);
  le32(q+88,data);le32(q+100,7);
  Mt6991UfsDmaResolve(&d,data,8,false,&cpu);be32(cpu,64577535);be32((unsigned char*)cpu+4,4096);
 }
 r=q+32;r[0]=0x21;r[2]=2;r[3]=0x72;
 switch(which) {
 case 1:trl+=1024;break;
 case 2:le32(trd+16,cmd+4096);break;
 case 3:le16(trd+24,0xfffe);break;
 case 4:le32(q+88,data+4);break;
 case 5:le32(q+92,1);break;
 case 7:Mt6991UfsDmaMap(&d,Mt6991DmaDeviceWrite,host+8,8,&data,&extra);break;
 case 8:q[16]=0x2a;break;
 case 9:q[2]=0xc4;break;
 case 10:le16(trd+30,8);break;
 case 11:r[3]++;break;
 case 12:r[2]++;break;
 case 13:r[1]=0x20;be32(r+12,9);break;
 case 14:be16(r+32,19);break;
 case 15:be32((unsigned char*)cpu+4,513);break;
 case 16:r[7]=2;break;
 case 17:r[1]=0x40;break;
 case 18:r[0]=0x36;break;
 case 19:le32(q+100,0xffffffff);break;
 default:break;
 }
 if(which>=20) {
  memset(q,0,4096);memset(r,0,32);le16(trd+28,0);le16(trd+24,8);q[3]=0x72;r[3]=0x72;
  le32(trd,0x11000000);r[0]=0x20;
  if(which>=21) {
   q[0]=0x16;q[5]=1;q[12]=1;q[13]=2;q[14]=2;be16(q+18,32);
   le32(trd,0x15000000);le16(trd+24,16);r[0]=0x36;r[5]=1;memcpy(r+12,q+12,4);be16(r+18,32);be16(r+10,32);
   if(which==22){q[12]=2;}
   if(which==23||which==24){q[5]=0x81;q[12]=6;q[13]=1;q[14]=0;be16(q+18,0);le32(trd,0x13000000);le16(trd+24,8);
    memset(r,0,32);r[0]=0x36;r[3]=0x72;r[5]=0x81;memcpy(r+12,q+12,4);}
   if(which==25){be16(r+18,33);}
   if(which==26){q[15]=1;}
  }
 }
 if(which>=28) {
  memset(q,0,4096);memset(r,0,32);le32(trd,0x13000000);le32(trd+8,15);le32(trd+16,cmd);
  le16(trd+24,14);le16(trd+26,8);le16(trd+28,1);le16(trd+30,22);
  q[16]=0x2a;be32(q+18,434432);be16(q+23,1);
  g[2]=(MT6991_UFS_GEOMETRY){64577536,4096,true,true,false};memset(host,0x5a,sizeof host);
  if(which!=28)grant_init(&grant);
  if(which>=60){
   grant.Target=MT6991_UFS_TARGET_USERDATA_LOGS;grant.BootStartLba=64565240;grant.BootBlocks=8192;
   grant.CandidateBytes=0;grant.Sequence=1;grant.ScratchStartLba=64565240;
   grant.ScratchBlocks=1024;grant.TailGuardBlocks=7168;
   const UINT8 logsGuid[16]={0xcd,0xe9,0xab,0x32,0x18,0xd8,0x47,0x77,0x8c,0xed,0x69,0x1e,0x7c,0xd5,0xc6,0x36};
   memcpy(grant.PartitionGuid,logsGuid,16);
  if(which==64)grant.Sequence=2;
   be32(q+18,which==61?64565240+1023:which==62?64565240+1024:which==63?64565240+1023:64565240);
   if(which==63)be16(q+23,2);
  }else if(which>=34){
   grant.Target=MT6991_UFS_TARGET_EXPDB;grant.BootStartLba=2184;grant.BootBlocks=32768;grant.CandidateBytes=0;
   grant.ScratchStartLba=2440;grant.ScratchBlocks=256;grant.TailGuardBlocks=32256;
   be32(q+18,which==35?2439:which==36?2696:which==37?2695:which==40?34951:2440);
   if(which==38)grant.Target=MT6991_UFS_TARGET_BOOT_B;
   if(which==39)grant.Target=2;
  }
  if(which==30)be32(q+18,434688);
  if(which==31)g[2].WriteProtectionKnown=false;
  if(which==32)q[17]=8;
  bool read=which==43||which==44;
  if(read){q[16]=0x28;le32(trd,0x15000000);}
  if(which==45){memset(q+16,0,16);q[16]=0x8a;be64(q+18,2440);be32(q+26,1);}
  unsigned char cdb[16];memcpy(cdb,q+16,16);memset(q,0,32);
  UfsInitCommandUpiu((UTP_COMMAND_UPIU*)q,2,0x72,cdb,which==45?16:10,read?UfsDataIn:UfsDataOut,4096);
  out[11]=q[1];
  if(which==41)q[1]=0x41; /* read flag on a write command */
  if(which==42)q[1]=0x61; /* simultaneous read and write flags */
  if(which==44)q[1]=0x21; /* write flag on a read command */
  Mt6991UfsDmaMap(&d,which==33||read?Mt6991DmaDeviceWrite:Mt6991DmaDeviceRead,host,sizeof host,&data,&id3);
  le32(q+88,data);le32(q+100,4095);r[0]=0x21;r[2]=2;r[3]=0x72;
 }
 out[0]=Mt6991UfsValidateTransfer(&d,trl,g,&grant,which!=24,&p);
 if(!out[0]) {
  if(which==29&&Mt6991UfsDmaResolve(&d,data,1,false,&cpu)==0)out[10]=((unsigned char*)cpu)[0];
  out[1]=Mt6991UfsDmaSubmit(&d);le32(trd+8,0);
  if(which==27)le32(trd+8,1);
  bool valid=false;out[2]=Mt6991UfsValidateResponse(&p,g,&valid);out[3]=valid;
  if(!out[2]){Mt6991UfsDmaComplete(&d,valid);if(id3)Mt6991UfsDmaUnmap(&d,id3);}
  out[4]=g[2].Blocks;out[5]=g[2].BlockSize;out[6]=g[2].Valid;out[7]=host[0];
 }
 Mt6991UfsDmaStopped(&d,0);out[8]=Mt6991UfsDmaReleaseAll(&d);out[9]=live;
}
''')
        pkg = Path(__file__).resolve().parents[2] / 'Silicon/MediaTek/MT6991Pkg'
        compiler = shutil.which('clang') or shutil.which('cc')
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(pkg / 'Include'),
                        str(pkg / 'Library/UfsDmaLib/UfsDma.c'), str(pkg / 'Library/UfsDmaLib/UfsDmaUefi.c'),
                        str(pkg / 'Drivers/UfsHostDxe/UfsPolicy.c'), str(base / 'policy.c'),
                        '-o', str(base / 'policy.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'policy.so'))
        cls.lib.wire.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.command.argtypes = [ctypes.c_uint] * 3 + [ctypes.c_uint64] * 2 + [ctypes.c_uint, ctypes.c_uint64] + [ctypes.c_uint] * 3
        cls.lib.command.restype = ctypes.c_uint64
        cls.lib.scratch.argtypes = [ctypes.c_uint]
        cls.lib.scratch.restype = ctypes.c_uint64
        cls.lib.scratch_map.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.scratch_map.restype = ctypes.c_uint64
        cls.lib.build_rw.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.build_rw.restype = ctypes.c_uint64
        cls.lib.Mt6991UfsBoundTimeout.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.Mt6991UfsBoundTimeout.restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls):
        dma_test.UfsDmaTests.tearDownClass()

    def wire(self, which):
        out = (ctypes.c_uint64 * 16)(); self.lib.wire(which, out)
        self.assertEqual(list(out)[8:10], [0, 0])
        return list(out)

    def test_capacity_comes_from_actual_response_on_lun_two(self):
        out = self.wire(0)
        self.assertEqual(out[:7], [0, 0, 0, 1, 64577536, 4096, 1])
        self.assertEqual(out[7], 3)

    def test_expdb_owned_descriptor_path_cannot_write_head_or_tail_or_reuse_boot_profile(self):
        for mode in (34,37):
            out=self.wire(mode)
            self.assertEqual(out[0:4],[0,0,0,1])
        for mode in (35,36,38,39,40):
            self.assertNotEqual(self.wire(mode)[0],0,mode)

    def test_userdata_logs_owned_descriptor_cannot_cross_into_xnu_slot_or_use_wrong_sequence(self):
        for mode in (60,61):
            out=self.wire(mode);self.assertEqual(out[0:4],[0,0,0,1],mode)
        for mode in (62,63,64):
            self.assertNotEqual(self.wire(mode)[0],0,mode)

    def test_actual_mu_command_upiu_direction_matches_read_write10_and_write16(self):
        for mode,flags in ((34,0x21),(43,0x41),(45,0x21)):
            with self.subTest(mode=mode):
                out=self.wire(mode)
                self.assertEqual(out[11],flags)
                self.assertEqual(out[:4],[0,0,0,1])
        for mode in (41,42,44):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.wire(mode)[0],0)

    def test_unowned_overlapping_and_hostile_descriptors_never_submit(self):
        for case in range(1, 11):
            with self.subTest(case=case): self.assertNotEqual(self.wire(case)[0], 0)
        self.assertNotEqual(self.wire(19)[0], 0)

    def test_malformed_completions_cannot_validate_data(self):
        for case in (11, 12, 13, 14, 15, 17, 18, 25, 27):
            with self.subTest(case=case):
                out = self.wire(case)
                self.assertEqual(out[0], 0)
                self.assertNotEqual(out[2], 0)

    def test_check_condition_is_transport_completion_without_copyback(self):
        out = self.wire(16)
        self.assertEqual(out[:4], [0, 0, 0, 0])
        self.assertEqual(out[6:8], [0, 0])

    def test_nop_read_descriptor_and_single_initialization_control(self):
        for case in (20, 21, 23): self.assertEqual(self.wire(case)[:4], [0, 0, 0, 0])
        for case in (22, 24, 26): self.assertNotEqual(self.wire(case)[0], 0)

    def command(self, op, *, block=4096, lba=2, count=1, blocks=16, size=None, ready=1, lun=2, cbytes=None, immediate=0):
        if cbytes is None: cbytes = 16 if op in (0x88, 0x91, 0x9e) else (6 if op in (0, 3, 0x12, 0x1a) else (12 if op == 0xa0 else 10))
        if size is None: size = block * count if op in (0x28, 0x88) else (8 if op == 0x25 else (32 if op == 0x9e else 0))
        return self.lib.command(op, lun, cbytes, lba, count, block, blocks, size, ready, immediate)

    def test_all_other_opcodes_and_well_known_luns_are_denied(self):
        allowed = {0, 3, 0x12, 0x1a, 0x25, 0x28, 0x35, 0x5a, 0x88, 0x91, 0x9e, 0xa0}
        for op in range(256):
            with self.subTest(op=op): self.assertEqual(self.command(op) == 0, op in allowed)
        for lun in (8, 0x81, 0xb0, 0xc4, 255): self.assertNotEqual(self.command(0x25, lun=lun), 0)

    def test_real_block_sizes_and_capacity_boundaries(self):
        for block in (512, 4096, 65536):
            self.assertEqual(self.command(0x88, block=block, lba=15), 0)
            self.assertNotEqual(self.command(0x88, block=block, lba=16), 0)
            self.assertNotEqual(self.command(0x88, block=block, size=block+1), 0)
        self.assertNotEqual(self.command(0x88, lba=2**64-1), 0)
        self.assertNotEqual(self.command(0x28, ready=0), 0)
        self.assertNotEqual(self.command(0x88, count=257, blocks=10000), 0)
        self.assertNotEqual(self.command(0x28, cbytes=6), 0)

    def test_sync_rejects_immediate_and_data_payloads(self):
        for op in (0x35, 0x91):
            self.assertEqual(self.command(op), 0)
            self.assertNotEqual(self.command(op, immediate=1), 0)
            self.assertNotEqual(self.command(op, size=1), 0)

    def test_scratch_write_requires_exact_geometry_nonzero_bindings_and_one_mib_window(self):
        self.assertEqual(self.lib.scratch(0), 0)
        self.assertEqual(self.lib.scratch(1), 0)  # WRITE(16) at final scratch block.
        for mode in (2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19):
            with self.subTest(mode=mode): self.assertEqual(self.lib.scratch(mode), 8)
        for mode in (5, 20, 21, 22, 23, 24, 25, 26):
            with self.subTest(mode=mode): self.assertEqual(self.lib.scratch(mode), 2)

    def test_userdata_logs_scratch_write_is_limited_to_its_uefi_four_mib_slot(self):
        self.assertEqual(self.lib.scratch(30),0)
        self.assertEqual(self.lib.scratch(31),0)
        for mode in (32,33,34,35,36):
            with self.subTest(mode=mode):self.assertEqual(self.lib.scratch(mode),8)

    def test_scratch_blockio_translation_is_bounded_and_requires_known_wp_for_writes(self):
        absolute = ctypes.c_uint64(); count = ctypes.c_uint64()
        self.assertEqual(self.lib.scratch_map(0, ctypes.byref(absolute), ctypes.byref(count)), 0)
        self.assertEqual((absolute.value, count.value), (434432, 1))
        self.assertEqual(self.lib.scratch_map(1, ctypes.byref(absolute), ctypes.byref(count)), 0)
        self.assertEqual((absolute.value, count.value), (434687, 1))
        self.assertEqual(self.lib.scratch_map(8, ctypes.byref(absolute), ctypes.byref(count)), 0)
        self.assertEqual((absolute.value, count.value), (434432, 1))
        for mode, status in ((2, 2), (3, 2), (4, 4), (5, 8), (6, 8), (7, 8), (9, 2), (10, 8)):
            with self.subTest(mode=mode):
                absolute.value = count.value = 123
                self.assertEqual(self.lib.scratch_map(mode, ctypes.byref(absolute), ctypes.byref(count)), status)
                self.assertEqual((absolute.value, count.value), (0, 0))

    def test_scratch_blockio_builds_exact_rw10_and_rw16_cdbs(self):
        for mode, expected, length in (
            (0, bytes.fromhex('28000006a10000000100'), 10),
            (1, bytes.fromhex('88000000000100000000000000010000'), 16),
            (2, bytes.fromhex('2a000006a10000000100'), 10),
            (3, bytes.fromhex('8a000000000100000000000000010000'), 16),
        ):
            cdb = (ctypes.c_ubyte * 16)(); cdb_bytes = ctypes.c_uint64()
            self.assertEqual(self.lib.build_rw(mode, cdb, ctypes.byref(cdb_bytes)), 0)
            self.assertEqual(cdb_bytes.value, length)
            self.assertEqual(bytes(cdb[:length]), expected)
        for mode in (4, 5, 6):
            cdb = (ctypes.c_ubyte * 16)(); cdb_bytes = ctypes.c_uint64()
            cdb_bytes.value = 9
            self.assertEqual(self.lib.build_rw(mode, cdb, ctypes.byref(cdb_bytes)), 2)
            self.assertEqual(cdb_bytes.value, 0)
            self.assertEqual(bytes(cdb), bytes(16))

    def test_scratch_wire_write_is_default_denied_and_authorized_data_is_staged_before_submission(self):
        denied = self.wire(28)
        self.assertEqual(denied[0], 8)
        self.assertEqual(denied[1], 0)  # no DMA submit without the grant
        allowed = self.wire(29)
        self.assertEqual(allowed[:4], [0, 0, 0, 1])
        self.assertEqual(allowed[10], 0x5A)  # DeviceRead bounce received host payload
        for mode in (30, 31, 32, 33):
            with self.subTest(mode=mode): self.assertNotEqual(self.wire(mode)[0], 0)

    def test_generic_scsi_timeout_is_bounded_without_rejecting_valid_reads(self):
        for supplied, expected in ((1, 1), (30000000, 30000000), (300000000, 30000000), (2**64-1, 30000000)):
            timeout = ctypes.c_uint64(supplied)
            self.assertEqual(self.lib.Mt6991UfsBoundTimeout(ctypes.byref(timeout)), 0)
            self.assertEqual(timeout.value, expected)
        timeout = ctypes.c_uint64(0)
        self.assertNotEqual(self.lib.Mt6991UfsBoundTimeout(ctypes.byref(timeout)), 0)
        self.assertNotEqual(self.lib.Mt6991UfsBoundTimeout(None), 0)
