"""Whole-file firmware measurement and exact RAM filesystem consumer, real Mu ABI."""
import ctypes as c
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


class UfsProvenanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[2]
        mu, pkg = root/'Mu_Basecore', root/'Silicon/MediaTek/MT6991Pkg'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
            '-Wno-missing-field-initializers', '-include', 'Uefi.h',
            '-I', str(mu/'MdePkg/Include'), '-I', str(mu/'MdePkg/Include/AArch64'),
            '-I', str(mu/'CryptoPkg/Include'), '-I', str(pkg/'Drivers/UfsHostDxe'),
            str(root/'tools/dali/ufs_provenance_harness.c'), str(pkg/'Drivers/UfsHostDxe/UfsProvenance.c'),
            '-o', str(Path(cls.temp.name)/'test.so')], check=True)
        cls.lib = c.CDLL(str(Path(cls.temp.name)/'test.so'))
        callback = c.CFUNCTYPE(c.c_ubyte, c.c_void_p, c.c_size_t, c.c_void_p)
        def digest(data, size, out):
            c.memmove(out, hashlib.sha256(c.string_at(data, size)).digest(), 32)
            return 1
        cls.backend = callback(digest)
        cls.lib.set_backend.argtypes = [callback]
        cls.lib.set_backend(cls.backend)
        cls.lib.run.argtypes = [c.c_uint, c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t,
                                c.POINTER(c.c_uint64), c.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def fixture(self):
        pins = {key: char*40 for key, char in zip(('device_commit','unixnu_commit','uefi_commit','xnu_commit'),'1234')}
        source = b'DALIUFSSRC1'+bytes(5)+b''.join(v.encode()+b'\0' for v in pins.values())
        kernel = bytearray(1024)
        struct.pack_into('<8I', kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 0, 0, 0, 0)
        kernel[253:253+len(source)] = source
        # Deliberately unaligned source record and trailer outside all segments.
        kernel[-7:] = b'TRAILER'
        return kernel, source, pins

    def run_measurement(self, fault=0, kernel=None, source=None):
        original, record, _ = self.fixture()
        kernel = bytes(original if kernel is None else kernel)
        source = record if source is None else source
        out, digests = (c.c_uint64*9)(), (c.c_ubyte*64)()
        self.lib.run(fault, kernel, len(kernel), source, len(source), out, digests)
        out = list(out)
        self.assertEqual(out[7], out[8], 'all allocations must be released')
        if out[0]:
            self.assertEqual(out[1], 0)
            self.assertEqual(bytes(digests), bytes(64), 'partial verification must not escape')
        return out, bytes(digests)

    def test_whole_file_and_canonical_source_hash_match_independent_host(self):
        kernel, source, pins = self.fixture()
        out, digests = self.run_measurement(kernel=kernel, source=source)
        self.assertEqual(out[:7], [0,1,2,2,2,1,1])
        measured = hashlib.sha256(kernel).digest()
        expected = hashlib.sha256(b'DALI-UFS-SOURCE-V1\0'+json.dumps(
            dict(pins, kernel_sha256=measured.hex()), sort_keys=True, separators=(',',':')).encode()).digest()
        self.assertEqual(digests, measured+expected)
        kernel[-1] ^= 1
        self.assertNotEqual(self.run_measurement(kernel=kernel)[1], digests)

    def test_missing_duplicate_partial_and_malformed_source_records_fail(self):
        for kind in ('missing','duplicate','partial','badhex','badnul','mismatch'):
            kernel, source, _ = self.fixture()
            if kind == 'missing': kernel[253:433] = bytes(180)
            if kind == 'duplicate': kernel[601:781] = source
            if kind == 'partial':
                kernel[253:433] = bytes(180)
                kernel[-16:] = source[:16]
            if kind == 'badhex': kernel[269] = ord('G')
            if kind == 'badnul': kernel[309] = 1
            if kind == 'mismatch': kernel[269] = ord('a')
            self.assertEqual(self.run_measurement(kernel=kernel)[0][0], 26, kind)

    def test_ordinary_marker_constant_is_not_a_second_compiled_record(self):
        kernel, source, _ = self.fixture()
        kernel[101:117] = source[:16]
        self.assertEqual(self.run_measurement(kernel=kernel)[0][0], 0)

    def test_compiled_source_is_checked_independently(self):
        kernel, source, _ = self.fixture()
        for record in (bytes(180), source[:-1], b'X'+source[1:], source[:16]+b'A'+source[17:]):
            self.assertEqual(self.run_measurement(kernel=kernel, source=record)[0][0], 26)

    def test_wrong_kernel_format_is_rejected(self):
        for offset in (0,4,12):
            kernel, _, _ = self.fixture(); kernel[offset] ^= 1
            self.assertEqual(self.run_measurement(kernel=kernel)[0][0], 3)

    def test_filesystem_selection_is_unique_exact_and_readonly(self):
        for fault, status in [(1,26),(2,14),(3,26),(4,26)]:
            out,_ = self.run_measurement(fault)
            self.assertEqual(out[0], status)
            self.assertEqual(out[2:7], [0]*5)

    def test_io_warnings_short_reads_changes_and_close_failures_never_hash(self):
        for fault in range(5,20):
            out,_ = self.run_measurement(fault)
            self.assertNotEqual(out[0], 0, fault)
            self.assertEqual(out[4], 0, fault)
            self.assertLessEqual(out[2], 2)

    def test_failed_hash_backend_clears_all_partial_state(self):
        for fault in (20,21):
            out,_ = self.run_measurement(fault)
            self.assertEqual(out[0], 7)
            self.assertEqual(out[4], fault-19)

    def test_malformed_file_info_is_refused(self):
        self.assertNotEqual(self.run_measurement(22)[0][0], 0)


if __name__ == '__main__':
    unittest.main()
