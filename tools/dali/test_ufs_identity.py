"""Execute production identity capture, Mu query helper and Dali callback with real headers."""
import ctypes
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from mu_patch_sources import patched_mu_source
from test_ufs_passthru_policy import function


class UfsIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        root = Path(__file__).resolve().parents[2]
        pkg = root/'Silicon/MediaTek/MT6991Pkg'
        mu = root/'Mu_Basecore'
        with patched_mu_source(root) as patched:
            interface = (patched/'MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThru.c').read_text()
            hci = (patched/'MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruHci.c').read_text()
        adapter = (pkg/'Drivers/UfsHostDxe/UfsHostDxe.c').read_text()
        prefix = r'''
#include "UfsPassThru.h"
#include "UfsIdentity.h"
#include <Library/Mt6991UfsPolicy.h>
#include <string.h>
#include <stdlib.h>
#define COMMAND_TIMEOUT MT6991_UFS_COMMAND_TIMEOUT_100NS
#undef FixedPcdGetPtr
#define FixedPcdGetPtr(Token) "00000000000000000000000000000001"
static DALI_UFS_IDENTITY mIdentity;
static MT6991_UFS_GEOMETRY mGeometry[8];
static MT6991_UFS_SCRATCH_GRANT mScratchGrant;
static BOOLEAN mClosed,mInitUsed,mInitFinished;
static EFI_HANDLE mController=(VOID*)1;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
static UINT32 Fault,Queries,Hashes,StopCalls;
static BOOLEAN (*HashBackend)(CONST VOID *,UINTN,UINT8 *);
VOID set_hash_backend(BOOLEAN (*Backend)(CONST VOID *,UINTN,UINT8 *)){HashBackend=Backend;}
static EFI_STATUS StopController(VOID){++StopCalls;return EFI_SUCCESS;}
VOID *EFIAPI ZeroMem(VOID *p,UINTN n){return memset(p,0,n);}
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n){return memcpy(d,s,n);}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(VOID)Level;return TRUE;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *Data,UINTN Bytes,UINT8 *Digest){
 ++Hashes;if(Fault==8){memset(Digest,0xAA,32);return FALSE;}
 return HashBackend!=NULL && HashBackend(Data,Bytes,Digest);
}
EFI_STATUS Mt6991UfsBoundTimeout(UINT64 *T){(VOID)T;return EFI_UNSUPPORTED;}
EFI_STATUS Mt6991UfsScratchWriteCommand(CONST MT6991_UFS_GEOMETRY *G,CONST MT6991_UFS_SCRATCH_GRANT *S,UINT8 L,CONST UINT8 *C,UINTN N,UINT32 B){(VOID)G;(VOID)S;(VOID)L;(VOID)C;(VOID)N;(VOID)B;return EFI_UNSUPPORTED;}
EFI_STATUS Mt6991UfsReadOnlyCommand(CONST MT6991_UFS_GEOMETRY *G,UINT8 L,CONST UINT8 *C,UINTN N,UINT32 B){(VOID)G;(VOID)L;(VOID)C;(VOID)N;(VOID)B;return EFI_UNSUPPORTED;}
EFI_STATUS Mt6991UfsReadOnlyQuery(UINT8 Op,UINT8 Id,UINT8 Ix,UINT8 Sel,UINT32 N,BOOLEAN Init){(VOID)Op;(VOID)Id;(VOID)Ix;(VOID)Sel;(VOID)N;(VOID)Init;return EFI_UNSUPPORTED;}
EFI_STATUS UfsRwDeviceDesc(UFS_PASS_THRU_PRIVATE_DATA *P,BOOLEAN Read,UINT8 Id,UINT8 Ix,UINT8 Sel,VOID *D,UINT32 *N){
 (VOID)P;if(!Read||Id!=5||Ix!=9||Sel||*N!=254)abort();
 ++Queries;UINT8 serial[6]={6,5,0,'A',0,'B'};memcpy(D,serial,6);*N=6;
 if(Fault==1)return EFI_TIMEOUT;
 if(Fault==2)return EFI_WARN_UNKNOWN_GLYPH;
 if(Fault==3)*N=3;
 if(Fault==4){((UINT8*)D)[0]=8;*N=6;}
 if(Fault==5)((UINT8*)D)[1]=4;
 if(Fault==6)((UINT8*)D)[0]=5;
 if(Fault==7)memset((UINT8*)D+2,0,4);
 if(Fault==9)*N=255;
 if(Fault==10){memset(D,0,254);((UINT8*)D)[0]=254;((UINT8*)D)[1]=5;((UINT8*)D)[253]=1;*N=254;}
 if(Fault==11)*N=254; // complete descriptor with extra returned padding
 return EFI_SUCCESS;
}
'''
        suffix = r'''
VOID identity_run(UINT32 Version,UINT32 Failure,UINT64 *Out,UINT8 *Digest){
 Fault=Failure;Queries=Hashes=StopCalls=0;mClosed=FALSE;
 memset(&mIdentity,0,sizeof mIdentity);
 EDKII_UFS_HC_PLATFORM_PROTOCOL Platform={0};Platform.Version=Version;Platform.Callback=Callback;
 mUfsHcPlatform=Failure==12?NULL:&Platform;
 if(Failure==13)Platform.Callback=NULL;
 UFS_PASS_THRU_PRIVATE_DATA Private={0};Private.Handle=(VOID*)1;
 UINT8 Device[sizeof(UFS_DEV_DESC)]={0};Device[0]=0x59;Device[22]=9;Device[24]=0x12;Device[25]=0x34;
 UINT32 Size=sizeof Device;
 if(Failure==14)Size=25;
 if(Failure==15)Device[22]=0;
 if(Failure==16)Device[22]=255;
 if(Failure==17)Device[1]=1;
 if(Failure==18)Size=sizeof Device+1;
 if(Failure==19)Device[0]=25;
 if(Failure==20)Private.Handle=(VOID*)2;
 if(Failure==21)mClosed=TRUE;
 if(Failure==23)Size=26;
 Out[0]=UfsCaptureDeviceIdentity(&Private,Device,Size);
 Out[1]=Queries;Out[2]=Hashes;Out[3]=mIdentity.Attempted;Out[4]=mIdentity.Ready;Out[5]=StopCalls;
 memcpy(Digest,mIdentity.Digest,32);
 if(Failure==22){Out[6]=UfsCaptureDeviceIdentity(&Private,Device,Size);Out[7]=mIdentity.Ready;Out[8]=Queries;Out[9]=Hashes;memcpy(Digest,mIdentity.Digest,32);}
}
VOID identity_direct(CONST UINT8 *D,UINT32 DN,CONST UINT8 *S,UINT32 SN,UINT64 *Out,UINT8 *Digest){
 Fault=Hashes=0;DALI_UFS_IDENTITY I={0};Out[0]=DaliUfsIdentityCapture(&I,D,DN,S,SN);
 Out[1]=Hashes;Out[2]=I.Attempted;Out[3]=I.Ready;memcpy(Digest,I.Digest,32);
}
'''
        (base/'identity.c').write_text(prefix + '\n' + function(hci, 'UfsHasCommandPolicy') + '\n' +
            function(hci, 'UfsCommandPolicy') + '\n' + function(adapter, 'Callback') + '\n' +
            function(interface, 'UfsCaptureDeviceIdentity') + suffix)
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-fshort-wchar',
            '-Wall', '-Wextra', '-Werror', '-include', 'Uefi.h',
            '-I', str(mu/'MdePkg/Include'), '-I', str(mu/'MdePkg/Include/AArch64'),
            '-I', str(mu/'MdeModulePkg/Include'), '-I', str(mu/'CryptoPkg/Include'),
            '-I', str(mu/'MdeModulePkg/Bus/Ufs/UfsPassThruDxe'),
            '-I', str(pkg/'Include'), '-I', str(pkg/'Drivers/UfsHostDxe'),
            str(base/'identity.c'), str(pkg/'Drivers/UfsHostDxe/UfsIdentity.c'),
            '-o', str(base/'identity.so')], check=True)
        cls.lib = ctypes.CDLL(str(base/'identity.so'))
        # Independent hash backend without adding an OS-specific libcrypto
        # development dependency. Production buffer construction stays in C.
        backend_type = ctypes.CFUNCTYPE(ctypes.c_ubyte, ctypes.c_void_p,
            ctypes.c_size_t, ctypes.c_void_p)
        def hash_backend(data, size, digest):
            ctypes.memmove(digest, hashlib.sha256(ctypes.string_at(data, size)).digest(), 32)
            return 1
        cls.hash_backend = backend_type(hash_backend)
        cls.lib.set_hash_backend.argtypes = [backend_type]
        cls.lib.set_hash_backend(cls.hash_backend)
        cls.lib.identity_run.argtypes = [ctypes.c_uint32, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_ubyte)]
        cls.lib.identity_direct.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p,
            ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_ubyte)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def case(self, fault=0, version=5):
        out, digest = (ctypes.c_uint64*10)(), (ctypes.c_ubyte*32)()
        self.lib.identity_run(version, fault, out, digest)
        return list(out), bytes(digest)

    def test_newer_device_descriptor_prefix_and_raw_hash_match_xnu(self):
        out, digest = self.case()
        self.assertEqual(out[:6], [0, 1, 1, 1, 1, 0])
        self.assertEqual(digest, hashlib.sha256(bytes.fromhex('1234060500410042')).digest())

    def test_legacy_or_absent_provider_adds_no_query(self):
        for version in (0, 3, 4):
            self.assertEqual(self.case(version=version), ([0]*10, bytes(32)))
        self.assertEqual(self.case(12), ([0]*10, bytes(32)))

    def test_minimum_valid_device_prefix_can_be_shorter_than_declared_length(self):
        self.assertEqual(self.case(23), self.case())

    def test_failures_never_replay_or_hash_incomplete_input(self):
        for fault in (1, 2, 3, 4, 5, 6, 7, 9):
            with self.subTest(fault=fault):
                out, digest = self.case(fault)
                self.assertNotEqual(out[0], 0)
                self.assertEqual(out[1:3], [1, 0])
                self.assertEqual(out[4], 0)
                self.assertEqual(digest, bytes(32))

    def test_crypto_failure_clears_partial_digest(self):
        out, digest = self.case(8)
        self.assertEqual(out[:5], [(1<<63)|7, 1, 1, 1, 0])
        self.assertEqual(digest, bytes(32))

    def test_maximum_serial_and_trailing_query_padding(self):
        out, digest = self.case(10)
        self.assertEqual(out[:5], [0, 1, 1, 1, 1])
        self.assertEqual(digest, hashlib.sha256(b'\x12\x34\xfe\x05' + bytes(251) + b'\x01').digest())
        self.assertEqual(self.case(11), self.case())

    def test_bad_prefix_or_missing_callback_refuses_before_query(self):
        for fault in range(13, 20):
            out, digest = self.case(fault)
            self.assertNotEqual(out[0], 0)
            self.assertEqual(out[1:5], [0]*4)
            self.assertEqual(digest, bytes(32))

    def test_wrong_controller_or_closed_host_never_captures_identity(self):
        for fault in (20, 21):
            out, digest = self.case(fault)
            self.assertNotEqual(out[0], 0)
            self.assertEqual(out[1:5], [1, 0, 0, 0])
            self.assertEqual(digest, bytes(32))

    def test_duplicate_capture_revokes_ready_instead_of_replacing_identity(self):
        out, digest = self.case(22)
        self.assertEqual(out[:5], [0, 1, 1, 1, 1])
        self.assertEqual(out[6:], [(1<<63)|20, 0, 2, 1])
        self.assertEqual(digest, bytes(32))

    def test_platform_independently_checks_lengths_headers_and_empty_serial(self):
        device = bytes([0x59, 0] + [0]*20 + [9, 0, 0x12, 0x34])
        serial = b'\x06\x05\x00A\x00B'
        for d, s in [(device[:25], serial), (device, serial[:3]),
            (device, b'\x08\x05\x00A\x00B'), (device, b'\x05\x05\x00A\x00'),
            (device, b'\x06\x04\x00A\x00B'), (device, b'\x06\x05\x00\x00\x00\x00'),
            (device, serial + bytes(249))]:
            out, digest = (ctypes.c_uint64*4)(), (ctypes.c_ubyte*32)()
            self.lib.identity_direct(d, len(d), s, len(s), out, digest)
            self.assertNotEqual(out[0], 0)
            self.assertEqual(list(out)[1:], [0, 1, 0])
            self.assertEqual(bytes(digest), bytes(32))


if __name__ == '__main__':
    unittest.main()
