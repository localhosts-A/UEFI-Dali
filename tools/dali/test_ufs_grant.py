"""Execute production prepare/authorize against hostile wire and observations."""
import ctypes as c
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid


class UfsGrantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc=shutil.which('clang') or shutil.which('cc')
        if not cc:raise unittest.SkipTest('native C compiler required')
        cls.temp=tempfile.TemporaryDirectory();base=Path(cls.temp.name)
        root=Path(__file__).resolve().parents[2];mu=root/'Mu_Basecore';pkg=root/'Silicon/MediaTek/MT6991Pkg';driver=pkg/'Drivers/UfsHostDxe'
        (base/'shim.c').write_text(r'''
#include <Uefi.h>
#include <string.h>
#include "UfsGrant.h"
VOID*EFIAPI CopyMem(VOID*d,CONST VOID*s,UINTN n){return memcpy(d,s,n);}
VOID*EFIAPI ZeroMem(VOID*d,UINTN n){return memset(d,0,n);}
INTN EFIAPI CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return memcmp(a,b,n);}
EFI_STATUS Mt6991UfsDmaResolve(MT6991_UFS_DMA*d,UINT64 a,UINTN n,BOOLEAN common,VOID**p){(VOID)d;(VOID)a;(VOID)n;(VOID)common;(VOID)p;return EFI_NOT_FOUND;}
VOID run(CONST UINT8*wire,UINTN bytes,UINT32 mode,UINT64*out){
 DALI_UFS_GRANT_STATE state={0};MT6991_UFS_SCRATCH_GRANT live={0};DALI_UFS_PROVENANCE provenance={0};
 provenance.Ready=TRUE;memset(provenance.CandidateSha256,0x22,32);memset(provenance.SourceSha256,0x33,32);
 CHAR8 token[33]="0123456789abcdef0123456789abcdef";
 if(mode==20)token[0]='f';if(mode==21)provenance.Ready=FALSE;
 out[0]=DaliUfsGrantPrepare(&state,wire,bytes,token,mode==22?32:sizeof token,&provenance);out[1]=state.Prepared;
 if(mode==16)out[0]=DaliUfsGrantPrepare(&state,wire,bytes,token,sizeof token,&provenance);
 MT6991_UFS_GEOMETRY geometry[8]={0};geometry[2]=(MT6991_UFS_GEOMETRY){64577536,4096,TRUE,TRUE,FALSE};
 UINT8 cdb[16];UINTN cdbBytes;Mt6991UfsBuildReadWriteCdb(TRUE,2440,1,cdb,&cdbBytes);
 out[2]=Mt6991UfsScratchWriteCommand(geometry,&live,2,cdb,cdbBytes,4096);
 DALI_UFS_IDENTITY identity={TRUE,TRUE,{0}};memset(identity.Digest,0x11,32);
 DALI_UFS_LAYOUT layout={0};layout.Verified=TRUE;layout.Target=MT6991_UFS_TARGET_EXPDB;layout.Lun=2;
 layout.BlockSize=4096;layout.CapacityBlocks=64577536;layout.BootStartLba=2184;layout.BootBlocks=32768;
 layout.ScratchStartLba=2440;layout.ScratchBlocks=256;layout.TailGuardBlocks=32256;
 const UINT8 guid[16]={0,0,0,3,0x39,0xc2,0x44,0x88,0x9b,0xb0,0,0xcb,0x43,0xc9,0xcc,0xd4};memcpy(layout.PartitionGuid,guid,16);
 switch(mode){case 1:identity.Ready=FALSE;break;case 2:identity.Digest[0]^=1;break;
 case 3:provenance.Ready=FALSE;break;case 4:provenance.SourceSha256[0]^=1;break;
 case 5:layout.Verified=FALSE;break;case 6:layout.Target=0;break;case 7:layout.PartitionGuid[0]^=1;break;
 case 8:geometry[2].WriteProtectionKnown=FALSE;break;case 9:geometry[2].WriteProtected=TRUE;break;
 case 10:--geometry[2].Blocks;break;case 11:geometry[2].BlockSize=512;break;
 case 12:++layout.BootStartLba;break;case 13:++layout.ScratchStartLba;break;
 case 18:identity.Attempted=FALSE;break;case 19:layout.Lun=1;break;default:break;}
 out[3]=DaliUfsGrantAuthorize(&state,&identity,&provenance,geometry,&layout,mode==14,&live);out[4]=live.Enabled;
 out[5]=Mt6991UfsScratchWriteCommand(geometry,&live,2,cdb,cdbBytes,4096);
 out[6]=DaliUfsGrantAuthorize(&state,&identity,&provenance,geometry,&layout,FALSE,&live);out[7]=live.Enabled;
 out[8]=Mt6991UfsScratchWriteCommand(geometry,&live,2,cdb,cdbBytes,4096);
 out[9]=state.Sequence;
}
''')
        subprocess.run([cc,'-shared','-fPIC','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror','-include','Uefi.h',
                        '-I',str(mu/'MdePkg/Include'),'-I',str(mu/'MdePkg/Include/AArch64'),
                        '-I',str(mu/'CryptoPkg/Include'),'-I',str(pkg/'Include'),'-I',str(driver),
                        str(driver/'UfsGrant.c'),str(driver/'UfsPolicy.c'),str(base/'shim.c'),'-o',str(base/'test.so')],check=True)
        cls.lib=c.CDLL(str(base/'test.so'));cls.lib.run.argtypes=[c.c_void_p,c.c_size_t,c.c_uint32,c.POINTER(c.c_uint64)]

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def wire(self):
        return struct.pack('<8sII32s32s32s32s16sIIQQQQQQQQI',b'DALIUFSW',3,2,
            b'0123456789abcdef0123456789abcdef',b'\x11'*32,b'\x22'*32,b'\x33'*32,
            uuid.UUID('00000003-39c2-4488-9bb0-00cb43c9ccd4').bytes,2,4096,64577536,
            2184,32768,0,2440,256,32256,1,1)

    def run_case(self,mode=0,wire=None):
        data=self.wire() if wire is None else bytes(wire);out=(c.c_uint64*10)()
        self.lib.run(data,len(data),mode,out);return [v&0x7fffffff for v in out]

    def test_only_all_independent_observations_enable_live_grant_and_repeat_revokes(self):
        self.assertEqual(self.run_case(),[0,1,8,0,1,0,15,0,8,1])

    def test_changed_identity_provenance_gpt_geometry_wp_and_closed_state_refuse(self):
        for mode in (*range(1,15),18,19):
            with self.subTest(mode=mode):
                r=self.run_case(mode);self.assertEqual(r[:3],[0,1,8]);self.assertNotEqual(r[3],0)
                self.assertEqual((r[4],r[5],r[7],r[8]),(0,8,0,8))

    def test_token_measurement_size_and_reprepare_refuse_without_live_grant(self):
        for mode in (16,20,21,22):
            with self.subTest(mode=mode):
                r=self.run_case(mode);self.assertNotEqual(r[0],0);self.assertEqual((r[4],r[5],r[7],r[8]),(0,8,0,8))

    def test_wrong_wire_version_flags_target_hashes_lun_geometry_extent_and_sequence_refuse(self):
        fields=((0,'B',0),(8,'I',2),(12,'I',1),(232,'I',0),(232,'I',2),
                (48,'B',0),(80,'B',0),(112,'B',0),(160,'I',8),(164,'I',0),
                (168,'Q',1),(176,'Q',2**64-1),(184,'Q',1),(192,'Q',1),
                (200,'Q',2441),(208,'Q',257),(216,'Q',256),(224,'Q',0),(224,'Q',129))
        for offset,fmt,value in fields:
            with self.subTest(offset=offset,value=value):
                data=bytearray(self.wire());struct.pack_into('<'+fmt,data,offset,value)
                r=self.run_case(wire=data)
                # Device identity is compared at authorize, the later live query.
                if offset==48:self.assertEqual(r[0],0)
                else:self.assertNotEqual(r[0],0)
                self.assertEqual((r[4],r[5]),(0,8))
        for data in (b'',self.wire()[:-4],self.wire()+b'\0'):
            self.assertNotEqual(self.run_case(wire=data)[0],0)
