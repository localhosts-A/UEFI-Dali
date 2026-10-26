"""Production expdb journal with real policy and independently inspected disk bytes."""
import ctypes as c
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


class UfsLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('clang') or shutil.which('cc')
        if not compiler:raise unittest.SkipTest('native compiler required')
        cls.temp=tempfile.TemporaryDirectory()
        root=Path(__file__).resolve().parents[2];mu=root/'Mu_Basecore'
        pkg=root/'Silicon/MediaTek/MT6991Pkg';driver=pkg/'Drivers/UfsHostDxe'
        target=Path(cls.temp.name)/'log.so'
        subprocess.run([compiler,'-shared','-fPIC','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-Wno-missing-field-initializers','-include','Uefi.h',
            '-I',str(mu/'MdePkg/Include'),'-I',str(mu/'MdePkg/Include/AArch64'),
            '-I',str(mu/'CryptoPkg/Include'),'-I',str(pkg/'Include'),'-I',str(driver),
            str(root/'tools/dali/ufs_log_harness.c'),str(driver/'UfsLog.c'),str(driver/'UfsPolicy.c'),
            '-o',str(target)],check=True)
        cls.lib=c.CDLL(str(target));callback=c.CFUNCTYPE(c.c_ubyte,c.c_void_p,c.c_size_t,c.c_void_p)
        def digest(data,size,out):
            c.memmove(out,hashlib.sha256(c.string_at(data,size)).digest(),32);return 1
        cls.backend=callback(digest);cls.lib.set_log_backend.argtypes=[callback];cls.lib.set_log_backend(cls.backend)
        cls.lib.run_log.argtypes=[c.c_uint,c.c_uint,c.c_ubyte,c.c_uint,c.c_uint64,c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p,c.c_void_p]

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def run_log(self,disk=None,fault=0,block=4096,readback=False,owner=1,sequence=1,body=b'DALI: journal\r\n'+bytes(range(256))*5):
        if disk is None:disk=bytearray(1048576)
        array=(c.c_ubyte*len(disk)).from_buffer(disk);out=(c.c_uint64*13)();digest=(c.c_ubyte*32)()
        self.lib.run_log(fault,block,readback,owner,sequence,array,None if readback else body,
                         0 if readback else len(body),out,digest)
        values=list(out)
        if values[4]:self.assertEqual(values[9],20,'single-use attempt never retries')
        self.assertEqual(values[1:4],values[10:13],'repeat call cannot submit more IO')
        return values,bytes(digest),disk

    def test_write_sync_and_fresh_boot_readback_have_identical_bound_log(self):
        for block in (512,4096,65536):
            out,digest,disk=self.run_log(block=block)
            self.assertEqual(out[:9],[0,2,3,3,1,1,1,1,1295])
            self.assertEqual(digest,hashlib.sha256(b'DALI: journal\r\n'+bytes(range(256))*5).digest())
            for offset,kind in ((0,1),(block,2),(1048576-block,3)):
                record=bytearray(disk[offset:offset+block]);self.assertEqual(record[:16],b'DALIUFSLOG1'+bytes(5))
                self.assertEqual(struct.unpack_from('<II',record,16),(kind,1))
                self.assertEqual(record[24:56],b'a'*32)
                self.assertEqual(struct.unpack_from('<IQIIQ',record,56),(1,1,1295,block,1048576//block))
                observed=record[212:244];record[212:244]=bytes(32)
                self.assertEqual(observed,hashlib.sha256(record).digest())
            second,again,_=self.run_log(disk,block=block,readback=True)
            self.assertEqual(second[:9],[0,1,0,0,1,0,0,1,1295]);self.assertEqual(digest,again)

    def test_each_write_sync_read_or_hash_failure_stops_without_replay(self):
        for fault in range(1,10):
            out,_,disk=self.run_log(fault=fault)
            self.assertNotEqual(out[0],0,fault);self.assertFalse(out[7],fault)
            self.assertLessEqual(out[2],3);self.assertLessEqual(out[3],3)
            if fault in (2,3,4,5):self.assertNotEqual(self.run_log(disk,readback=True)[0][0],0)

    def test_existing_or_incomplete_log_never_gets_overwritten(self):
        disk=bytearray(1048576);disk[900000]=1
        out,_,after=self.run_log(disk)
        self.assertEqual(out[0],20);self.assertEqual(out[2:4],[0,0]);self.assertEqual(after[900000],1)
        self.assertNotEqual(self.run_log(readback=True)[0][0],0)

    def test_corrupt_bound_records_body_padding_and_commit_are_rejected(self):
        _,_,original=self.run_log()
        for at in (0,16,20,24,56,60,68,72,76,84,116,148,180,212,300,
                   4096,8192,8192+1295,1048576-1):
            disk=bytearray(original);disk[at]^=1
            out,_,_=self.run_log(disk,readback=True)
            self.assertNotEqual(out[0],0,at);self.assertEqual(out[2:4],[0,0])
        for kwargs in (dict(sequence=2),dict(owner=2),dict(fault=13)):
            self.assertNotEqual(self.run_log(bytearray(original),readback=True,**kwargs)[0][0],0)

    def test_media_profile_and_payload_extent_refuse_before_submission(self):
        for kwargs in (dict(fault=10),dict(fault=11),dict(fault=12),dict(owner=0),dict(sequence=0),
                       dict(body=b''),dict(body=bytes(1048576))):
            out,_,_=self.run_log(**kwargs);self.assertNotEqual(out[0],0);self.assertEqual(out[1:4],[0,0,0])


if __name__=='__main__':unittest.main()
