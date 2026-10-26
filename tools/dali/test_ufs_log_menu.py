import ctypes as c
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LogMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('clang') or shutil.which('cc')
        if not compiler:raise unittest.SkipTest('native compiler required')
        cls.temp=tempfile.TemporaryDirectory();root=Path(__file__).resolve().parents[2]
        inc=root/'Mu_Basecore/MdePkg/Include';pkg=root/'Silicon/MediaTek/MT6991Pkg'
        bds=root/'Platforms/Xiaomi/daliPkg/Drivers/DiagnosticBdsDxe';target=Path(cls.temp.name)/'menu.so'
        subprocess.run([compiler,'-shared','-fPIC','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-Wno-missing-field-initializers','-include','Uefi.h','-I',str(inc),'-I',str(inc/'AArch64'),
            '-I',str(pkg/'Include'),'-I',str(bds),str(root/'tools/dali/ufs_log_menu_harness.c'),
            str(bds/'DaliUfsLogMenu.c'),'-o',str(target)],check=True)
        cls.lib=c.CDLL(str(target));cls.lib.run_menu.argtypes=[c.c_uint,c.c_void_p]

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def case(self,mode):
        out=(c.c_uint64*5)();self.lib.run_menu(mode,out);return list(out)

    def test_default_readback_write_requires_selection_and_confirmation(self):
        self.assertEqual(self.case(0)[:3],[0,1,2])
        self.assertEqual(self.case(1)[:3],[0,1,1])
        self.assertEqual(self.case(2)[:3],[0,1,2])
        self.assertEqual(self.case(15)[:3],[0,1,1])
        self.assertEqual(self.case(16)[:3],[0,1,2])
        for mode in (3,4):
            self.assertEqual(self.case(mode)[:4],[21,0,0,3000])

    def test_missing_wrong_or_warning_protocol_never_invokes_log(self):
        for mode in (5,6,7,8,10,13,14):
            result=self.case(mode);self.assertNotEqual(result[0],0);self.assertEqual(result[1:3],[0,0])

    def test_display_failure_cannot_submit_invisible_write_and_run_error_propagates(self):
        for mode in (11,12):self.assertEqual(self.case(mode)[:3],[7,0,0])
        self.assertEqual(self.case(9)[:3],[7,1,2])


if __name__=='__main__':unittest.main()
