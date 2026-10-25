"""Run the production wire guard with real DMA ownership and hostile descriptors."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import unittest

import test_ufs_dma as dma_test


class UfsHostPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dma_test.UfsDmaTests.setUpClass()
        base = Path(dma_test.UfsDmaTests.temp.name)
        with (base / 'Uefi.h').open('a') as f:
            f.write('\ntypedef uint16_t UINT16;\n#define EFI_WRITE_PROTECTED 8\n')
        with (base / 'Library/BaseMemoryLib.h').open('a') as f:
            f.write('#define CompareMem memcmp\n')
        (base / 'policy.c').write_text(r'''
#include "harness.c"
#include <Library/Mt6991UfsPolicy.h>
static void le16(unsigned char *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void le32(unsigned char *p,unsigned n){le16(p,n);le16(p+2,n>>16);}
static void be16(unsigned char *p,unsigned n){p[0]=n>>8;p[1]=n;}
static void be32(unsigned char *p,unsigned n){be16(p,n>>16);be16(p+2,n);}
static void be64(unsigned char *p,uint64_t n){be32(p,n>>32);be32(p+4,n);}
uint64_t command(unsigned op,unsigned lun,unsigned cbytes,uint64_t lba,uint64_t count,
                 unsigned block,uint64_t blocks,unsigned bytes,unsigned ready,unsigned immediate) {
 MT6991_UFS_GEOMETRY g[8]={0};unsigned char cdb[16]={0};
 if(lun<8)g[lun]=(MT6991_UFS_GEOMETRY){blocks,block,ready!=0};
 cdb[0]=op;cdb[1]=immediate?2:0;
 if(op==0x28){be32(cdb+2,lba);be16(cdb+7,count);}
 if(op==0x88){be64(cdb+2,lba);be32(cdb+10,count);}
 if(op==0x9e){cdb[1]=0x10;be32(cdb+10,bytes);}
 return Mt6991UfsReadOnlyCommand(g,lun,cdb,cbytes,bytes);
}
void wire(unsigned which,uint64_t *out) {
 scenario=0;live=allocs=frees=restores=ncsets=flushes=fences=restore_failed=0;
 memset(allocated,0,sizeof allocated);
 MT6991_UFS_DMA d={0};MT6991_UFS_GEOMETRY g[8]={0};MT6991_UFS_PENDING p;
 unsigned char *trd=NULL,*q=NULL,*r=NULL,host[64]={0};void *cpu;
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
 out[0]=Mt6991UfsValidateTransfer(&d,trl,g,which!=24,&p);
 if(!out[0]) {
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
