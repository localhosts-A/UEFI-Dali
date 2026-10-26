"""Execute the actual optional protocol method, with the console engine as a boundary."""
import ctypes as c
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_ufs_passthru_policy import function


class LogAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('clang') or shutil.which('cc')
        if not compiler:raise unittest.SkipTest('native compiler required')
        cls.temp=tempfile.TemporaryDirectory();root=Path(__file__).resolve().parents[2]
        mu=root/'Mu_Basecore';pkg=root/'Silicon/MediaTek/MT6991Pkg';driver=pkg/'Drivers/UfsHostDxe'
        source=(driver/'UfsHostDxe.c').read_text()
        prefix=r'''
#include <string.h>
#include <stdlib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Protocol/DaliUfsLog.h>
#include "UfsLogConsole.h"
#include "UfsGrant.h"
EFI_BOOT_SERVICES *gBS;
static BOOLEAN enabled,mClosed,mQuiescing,mQuiesced,mWritesInhibited;
static DALI_UFS_LOG_PROTOCOL mLogProtocol;
static DALI_UFS_LOG_CONSOLE mLogConsole;
static MT6991_UFS_SCRATCH_GRANT mScratchGrant;
static DALI_UFS_GRANT_STATE mGrantState;
static DALI_UFS_PROVENANCE mProvenance;
static DALI_UFS_IDENTITY mIdentity;
static MT6991_UFS_DMA mDma;
static unsigned fault,core_calls,seals,tpl;
#define FixedPcdGetBool(x) enabled
#define FixedPcdGetPtr(x) "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
VOID EFIAPI DebugPrint(UINTN level,CONST CHAR8 *fmt,...){(void)level;(void)fmt;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN level){(void)level;return TRUE;}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return tpl;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL n){EFI_TPL old=tpl;tpl=n;return old;}
static VOID EFIAPI restore_tpl(EFI_TPL n){tpl=n;}
EFI_STATUS DaliUfsScratchSealReadOnly(CONST MT6991_UFS_SCRATCH_GRANT *g){
 if(g!=&mScratchGrant||!mWritesInhibited||tpl!=TPL_APPLICATION)abort();++seals;
 return fault==12?EFI_MEDIA_CHANGED:EFI_SUCCESS;
}
EFI_STATUS DaliUfsLogConsoleRun(DALI_UFS_LOG_CONSOLE *s,CONST MT6991_UFS_SCRATCH_GRANT *g,
 UINT64 sequence,BOOLEAN readback,CONST DALI_RAM_CONSOLE *ring,UINTN capacity){
 if(s!=&mLogConsole||g!=&mScratchGrant||sequence!=1||tpl!=TPL_APPLICATION||
  (UINTN)ring!=DALI_CONSOLE_BASE||capacity!=DALI_CONSOLE_SIZE-12||
  (readback&&(!mWritesInhibited||seals!=1)))abort();
 ++core_calls;s->Attempted=TRUE;s->Record.Verified=fault!=13;
 return fault==13?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
'''
        suffix=r'''
VOID run_adapter(unsigned which,UINT64 *out){
 fault=which;core_calls=seals=0;tpl=TPL_APPLICATION;enabled=TRUE;
 mClosed=mQuiescing=mQuiesced=mWritesInhibited=FALSE;
 memset(&mLogConsole,0,sizeof mLogConsole);memset(&mDma,0,sizeof mDma);
 mScratchGrant.Enabled=TRUE;mGrantState.Prepared=TRUE;mGrantState.Sequence=1;mProvenance.Ready=mIdentity.Ready=TRUE;
 EFI_BOOT_SERVICES bs={.RaiseTPL=raise_tpl,.RestoreTPL=restore_tpl};gBS=&bs;
 if(which==1)enabled=FALSE;if(which==2)mClosed=TRUE;if(which==3)mQuiescing=TRUE;
 if(which==4)mQuiesced=TRUE;if(which==5)mScratchGrant.Enabled=FALSE;
 if(which==6)mGrantState.Prepared=FALSE;if(which==7)mProvenance.Ready=FALSE;
 if(which==8)mIdentity.Ready=FALSE;if(which==9)tpl=TPL_NOTIFY;
 if(which==10)mDma.Busy=TRUE;if(which==11)mLogConsole.Attempted=TRUE;
 UINT32 mode=which==14?DALI_UFS_LOG_WRITE:DALI_UFS_LOG_READBACK;
 out[0]=RunConsoleLog(which==15?NULL:&mLogProtocol,which==16?99:mode)&0x7fffffffffffffffULL;
 out[1]=core_calls;out[2]=seals;out[3]=mWritesInhibited;out[4]=tpl;
 out[5]=RunConsoleLog(&mLogProtocol,DALI_UFS_LOG_WRITE)&0x7fffffffffffffffULL;
 out[6]=core_calls;
}
'''
        code=Path(cls.temp.name)/'adapter.c';code.write_text(prefix+function(source,'RunConsoleLog')+suffix)
        target=Path(cls.temp.name)/'adapter.so'
        subprocess.run([compiler,'-shared','-fPIC','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-Wno-misleading-indentation','-include','Uefi.h','-I',str(mu/'MdePkg/Include'),
            '-I',str(mu/'MdePkg/Include/AArch64'),'-I',str(mu/'CryptoPkg/Include'),'-I',str(pkg/'Include'),
            '-I',str(root/'Silicon/Silicium/SiliciumPkg/Include'),
            '-I',str(driver),str(code),'-o',str(target)],check=True)
        cls.lib=c.CDLL(str(target));cls.lib.run_adapter.argtypes=[c.c_uint,c.c_void_p]

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def case(self,mode):
        out=(c.c_uint64*7)();self.lib.run_adapter(mode,out);return list(out)

    def test_readback_seals_all_write_gates_before_console_and_cannot_reopen(self):
        out=self.case(0);self.assertEqual(out[:5],[0,1,1,1,4]);self.assertEqual(out[5:],[20,1])
        out=self.case(14);self.assertEqual(out[:5],[0,1,0,0,4]);self.assertEqual(out[5:],[20,1])

    def test_disabled_wrong_identity_unready_busy_or_reentrant_never_calls_console(self):
        for mode in range(1,12):
            out=self.case(mode);self.assertNotEqual(out[0],0);self.assertEqual(out[1:3],[0,0])
        for mode in (15,16):
            out=self.case(mode);self.assertEqual(out[:3],[15,0,0])

    def test_seal_failure_is_sticky_and_console_failure_propagates(self):
        out=self.case(12);self.assertEqual(out[:4],[13,0,1,1]);self.assertEqual(out[5:],[8,0])
        out=self.case(13);self.assertEqual(out[:4],[7,1,1,1]);self.assertEqual(out[5:],[20,1])


if __name__=='__main__':unittest.main()
