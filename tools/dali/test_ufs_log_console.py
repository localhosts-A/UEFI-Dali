import ctypes as c
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LogConsoleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('clang') or shutil.which('cc')
        if not compiler:raise unittest.SkipTest('native compiler required')
        cls.temp=tempfile.TemporaryDirectory();root=Path(__file__).resolve().parents[2]
        mu=root/'Mu_Basecore';pkg=root/'Silicon/MediaTek/MT6991Pkg';driver=pkg/'Drivers/UfsHostDxe'
        target=Path(cls.temp.name)/'console.so'
        subprocess.run([compiler,'-shared','-fPIC','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-Wno-missing-field-initializers','-include','Uefi.h','-I',str(mu/'MdePkg/Include'),
            '-I',str(mu/'MdePkg/Include/AArch64'),'-I',str(mu/'CryptoPkg/Include'),'-I',str(pkg/'Include'),
            '-I',str(driver),str(root/'tools/dali/ufs_log_console_harness.c'),str(driver/'UfsLog.c'),
            str(driver/'UfsLogConsole.c'),str(driver/'UfsPolicy.c'),'-o',str(target)],check=True)
        cls.lib=c.CDLL(str(target));callback=c.CFUNCTYPE(c.c_ubyte,c.c_void_p,c.c_size_t,c.c_void_p)
        def digest(data,size,out):c.memmove(out,hashlib.sha256(c.string_at(data,size)).digest(),32);return 1
        cls.backend=callback(digest);cls.lib.set_console_backend.argtypes=[callback];cls.lib.set_console_backend(cls.backend)
        cls.lib.run_console.argtypes=[c.c_uint,c.c_void_p,c.c_void_p]

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def case(self,mode):
        out=(c.c_uint64*18)();digest=(c.c_ubyte*32)();self.lib.run_console(mode,out,digest)
        values=list(out);self.assertEqual(values[15],values[16]);self.assertEqual(values[17],4)
        self.assertEqual(values[8],20)
        return values,bytes(digest)

    def test_raw_partial_and_wrapped_ring_are_exact_with_real_journal(self):
        for mode in (0,8):
            out,digest=self.case(mode)
            self.assertEqual(out[:6],[0,2,3,3,1,1]);self.assertEqual(out[7],1)
            raw=bytes(i%251 for i in range(1024))
            expected=raw[:700] if mode==0 else raw[317:]+raw[:317]
            self.assertEqual(out[6],len(expected));self.assertEqual(digest,hashlib.sha256(expected).digest())
            self.assertEqual(out[9:15],[0,1,0,0,0,1])

    def test_unauthorized_allocation_empty_corrupt_and_overflow_ring_never_write(self):
        for mode,status in [(1,15),(2,9),(3,9),(4,14),(5,5),(6,33),(9,33)]:
            out,_=self.case(mode);self.assertEqual(out[0],status,mode)
            self.assertEqual(out[1:4],[0,0,0]);self.assertEqual(out[5],0)

    def test_revocation_during_allocation_rejects_first_io_and_cannot_rearm(self):
        out,_=self.case(7);self.assertEqual(out[:4],[12,1,0,0]);self.assertEqual(out[5],0)


if __name__=='__main__':unittest.main()
