"""Exercise the production GPT reader against independently encoded, hostile media."""
import ctypes as c
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid
import zlib


class Geometry(c.Structure):
    _fields_ = [('blocks', c.c_uint64), ('block', c.c_uint32), ('valid', c.c_ubyte),
                ('wp_known', c.c_ubyte), ('wp', c.c_ubyte)]


class Grant(c.Structure):
    _fields_ = [('enabled', c.c_ubyte), ('lun', c.c_ubyte), ('token', c.c_char * 33),
                ('device', c.c_ubyte * 32), ('candidate', c.c_ubyte * 32),
                ('source', c.c_ubyte * 32), ('guid', c.c_ubyte * 16), ('block', c.c_uint32),
                ('capacity', c.c_uint64), ('boot_start', c.c_uint64), ('boot_blocks', c.c_uint64),
                ('candidate_bytes', c.c_uint64), ('scratch_start', c.c_uint64),
                ('scratch_blocks', c.c_uint64), ('guard_blocks', c.c_uint64)]


class Layout(c.Structure):
    _fields_ = [('verified', c.c_ubyte), ('lun', c.c_ubyte), ('block', c.c_uint32),
                ('entry_count', c.c_uint32), ('entry_crc', c.c_uint32),
                ('primary_crc', c.c_uint32), ('backup_crc', c.c_uint32),
                ('capacity', c.c_uint64), ('boot_start', c.c_uint64), ('boot_blocks', c.c_uint64),
                ('scratch_start', c.c_uint64), ('scratch_blocks', c.c_uint64), ('guard_blocks', c.c_uint64),
                ('guid', c.c_ubyte * 16)]


READ = c.CFUNCTYPE(c.c_uint64, c.c_void_p, c.c_ubyte, c.c_uint64, c.c_size_t, c.c_void_p)
ERROR = 1 << 63
WORK_BYTES = 2 * 65536 + 2 * 128 * 1024


class UfsLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        root = Path(__file__).resolve().parents[2]
        cls.driver = root / 'Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe'
        cls.pkg = cls.driver.parents[1]
        mu = root / 'Mu_Basecore'
        (base/'shim.c').write_text(r'''
#include <Uefi.h>
#include <string.h>
#include "UfsLayout.h"
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
EFI_STATUS Mt6991UfsDmaResolve(MT6991_UFS_DMA *D,UINT64 A,UINTN N,BOOLEAN C,VOID **P){
 (VOID)D;(VOID)A;(VOID)N;(VOID)C;(VOID)P;return EFI_NOT_FOUND;
}
UINTN geometry_size(VOID){return sizeof(MT6991_UFS_GEOMETRY);}
UINTN grant_size(VOID){return sizeof(MT6991_UFS_SCRATCH_GRANT);}
UINTN layout_size(VOID){return sizeof(DALI_UFS_LAYOUT);}
''')
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
            '-include', 'Uefi.h', '-I', str(mu/'MdePkg/Include'), '-I', str(mu/'MdePkg/Include/AArch64'),
            '-I', str(cls.pkg/'Include'), '-I', str(cls.driver),
            str(cls.driver/'UfsLayout.c'), str(cls.driver/'UfsPolicy.c'), str(base/'shim.c'),
            '-o', str(base/'layout.so')], check=True)
        cls.lib = c.CDLL(str(base/'layout.so'))
        cls.lib.DaliUfsInspectScratchLayout.argtypes = [c.POINTER(Geometry), c.POINTER(Grant), READ,
            c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(Layout)]
        cls.lib.DaliUfsInspectScratchLayout.restype = c.c_uint64
        for name, kind in [('geometry_size', Geometry), ('grant_size', Grant), ('layout_size', Layout)]:
            function = getattr(cls.lib, name)
            function.restype = c.c_size_t
            if function() != c.sizeof(kind):
                raise AssertionError(name + ' disagrees with real firmware header')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def media(self, block=4096, count=4, stride=128, capacity=64577536, boot_start=418560):
        self.block, self.capacity = block, capacity
        self.boot_guid = uuid.UUID('00000046-39c2-4488-9bb0-00cb43c9ccd4')
        self.disk_guid = uuid.UUID(int=0)  # Real Android may intentionally use zero disk GUID.
        self.entries = bytearray(count*stride)
        self.count, self.stride = count, stride
        self.boot_blocks = 64*1024*1024//block
        self.boot_start = boot_start
        self.entry(0, self.boot_start, self.boot_start+self.boot_blocks-1, 'boot_b', self.boot_guid)
        self.entry(1, self.boot_start+self.boot_blocks, self.boot_start+self.boot_blocks+10,
                   'other', uuid.UUID(int=2))
        self.array_blocks = (len(self.entries)+block-1)//block
        self.first, self.last = 2+self.array_blocks, capacity-2-self.array_blocks
        self.primary_entries, self.backup_entries = 2, capacity-1-self.array_blocks
        self.primary_override = self.backup_override = None
        self.disk = {}
        self.geometry = (Geometry*8)()
        self.geometry[3] = Geometry(capacity, block, 1, 1, 0)
        self.grant = Grant(enabled=1, lun=3, token=b'0123456789abcdef0123456789abcdef',
            block=block, capacity=capacity, boot_start=boot_start, boot_blocks=self.boot_blocks,
            candidate_bytes=5*1024*1024, scratch_start=boot_start+self.boot_blocks-2*1024*1024//block,
            scratch_blocks=1024*1024//block, guard_blocks=1024*1024//block)
        for field in ('device', 'candidate', 'source'):
            getattr(self.grant, field)[:] = bytes([1])*32
        self.grant.guid[:] = self.boot_guid.bytes
        self.reads, self.fail_at, self.fail_status, self.change_at = [], None, ERROR|18, None
        self.rebuild()

    def entry(self, index, first, last, name, guid, attrs=0):
        stride = self.stride
        offset = index*stride
        raw = bytearray(stride)
        raw[:16] = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le
        raw[16:32] = guid.bytes_le
        struct.pack_into('<QQQ', raw, 32, first, last, attrs)
        encoded = name.encode('utf-16le')
        raw[56:56+len(encoded)] = encoded
        self.entries[offset:offset+stride] = raw

    def header(self, primary):
        raw = bytearray(self.block)
        raw[:8] = b'EFI PART'
        struct.pack_into('<IIIIQQQQ16sQIII', raw, 8, 0x10000, 92, 0, 0,
            1 if primary else self.capacity-1, self.capacity-1 if primary else 1,
            self.first, self.last, self.disk_guid.bytes_le,
            self.primary_entries if primary else self.backup_entries, self.count, self.stride,
            zlib.crc32(self.entries))
        struct.pack_into('<I', raw, 16, zlib.crc32(raw[:92]))
        return raw

    def rebuild(self):
        self.disk = {1: self.header(True), self.capacity-1: self.header(False)}
        padded = self.entries + bytes((-len(self.entries)) % self.block)
        self.disk[self.primary_entries] = bytearray(padded)
        self.disk[self.backup_entries] = bytearray(padded)

    def corrupt_header(self, primary, offset, fmt, value):
        raw = self.disk[1 if primary else self.capacity-1]
        struct.pack_into(fmt, raw, offset, value)
        size = struct.unpack_from('<I', raw, 12)[0]
        if 92 <= size <= len(raw):
            struct.pack_into('<I', raw, 16, 0)
            struct.pack_into('<I', raw, 16, zlib.crc32(raw[:size]))

    def inspect(self, workspace_bytes=WORK_BYTES):
        def read(context, lun, lba, size, buffer):
            self.reads.append((lun, lba, size))
            if len(self.reads) == self.fail_at:
                return self.fail_status
            data = self.disk.get(lba)
            if lun != self.grant.lun or data is None or len(data) != size:
                return ERROR|7
            c.memmove(buffer, bytes(data), size)
            if len(self.reads) == self.change_at:
                self.geometry[self.grant.lun].blocks -= 1
            return 0
        callback = READ(read)
        work = c.create_string_buffer(WORK_BYTES+32)
        c.memset(work, 0xA5, len(work))
        observed = Layout()
        c.memset(c.byref(observed), 0xFF, c.sizeof(observed))
        status = self.lib.DaliUfsInspectScratchLayout(self.geometry, c.byref(self.grant), callback,
            None, c.addressof(work)+16, workspace_bytes, c.byref(observed))
        self.assertEqual(work.raw[:16], b'\xA5'*16)
        self.assertEqual(work.raw[-16:], b'\xA5'*16)
        if status:
            self.assertEqual(bytes(observed), bytes(c.sizeof(observed)), 'failure leaked verified layout')
        return status, observed

    def test_real_block_sizes_and_actual_lun(self):
        for block in (512, 4096, 65536):
            with self.subTest(block=block):
                self.media(block)
                status, out = self.inspect()
                self.assertEqual(status, 0)
                self.assertEqual((out.verified, out.lun, out.block, out.boot_start, out.boot_blocks),
                    (1, 3, block, self.boot_start, self.boot_blocks))
                self.assertEqual(bytes(out.guid), self.boot_guid.bytes)
                self.assertEqual(out.scratch_start, self.grant.scratch_start)
                self.assertEqual(self.reads, [(3, 1, block), (3, self.capacity-1, block),
                    (3, self.primary_entries, self.array_blocks*block),
                    (3, self.backup_entries, self.array_blocks*block)])

    def test_64_bit_media_lbas_and_nondefault_entry_stride(self):
        self.media(512, stride=256, capacity=0x200000000, boot_start=0x100000000)
        self.assertEqual(self.inspect()[0], 0)

    def test_maximum_bounded_entry_array(self):
        self.media(512, count=1024, stride=128)
        self.assertEqual(self.inspect()[0], 0)
        self.assertEqual(self.reads[-1][2], 128*1024)

    def test_invalid_or_write_protected_geometry_reads_nothing(self):
        for field, value in [('valid', 0), ('wp_known', 0), ('wp', 1), ('block', 513), ('blocks', 123)]:
            self.media(); setattr(self.geometry[3], field, value)
            self.assertNotEqual(self.inspect()[0], 0)
            self.assertEqual(self.reads, [])

    def test_timeout_warning_and_media_change_are_single_attempt(self):
        for at in range(1, 5):
            for mode in ('timeout', 'warning', 'changed'):
                self.media()
                if mode == 'changed': self.change_at = at
                else: self.fail_at, self.fail_status = at, ERROR|18 if mode == 'timeout' else 1
                self.assertNotEqual(self.inspect()[0], 0)
                self.assertEqual(len(self.reads), at)

    def test_bad_header_crc_and_reciprocal_fields(self):
        for primary in (True, False):
            self.media(); self.disk[1 if primary else self.capacity-1][16] ^= 1
            self.assertNotEqual(self.inspect()[0], 0)
            for offset, fmt, value in [(8, '<I', 0x20000), (12, '<I', 91), (20, '<I', 1),
                (24, '<Q', 6), (32, '<Q', 7), (40, '<Q', 1), (48, '<Q', self.capacity),
                (80, '<I', 0), (80, '<I', 1025), (84, '<I', 129), (84, '<I', 8192)]:
                self.media(); self.corrupt_header(primary, offset, fmt, value)
                self.assertNotEqual(self.inspect()[0], 0, (primary, offset, value))
                self.assertEqual(len(self.reads), 2)

    def test_arrays_must_stay_outside_usable_area_and_headers(self):
        for primary, lba in [(True, 1), (True, 3), (False, 2), (False, 0xffffffffffffffff)]:
            self.media(); self.corrupt_header(primary, 72, '<Q', lba)
            self.assertNotEqual(self.inspect()[0], 0)
            self.assertEqual(len(self.reads), 2)

    def test_entry_crc_and_primary_backup_byte_identity(self):
        self.media(); self.disk[self.backup_entries][56] ^= 1
        self.assertNotEqual(self.inspect()[0], 0)
        # Valid CRC on each table is insufficient when the tables disagree.
        raw = self.disk[self.capacity-1]
        struct.pack_into('<I', raw, 88, zlib.crc32(self.disk[self.backup_entries][:len(self.entries)]))
        self.corrupt_header(False, 88, '<I', struct.unpack_from('<I', raw, 88)[0])
        self.reads.clear()
        self.assertNotEqual(self.inspect()[0], 0)

    def test_overlap_duplicate_guid_and_out_of_bounds_partitions(self):
        for first, last, guid in [(self_start:=418560, self_start+2, uuid.UUID(int=2)),
            (1, 2, uuid.UUID(int=2)), (self_start+20000, self_start+20001,
             uuid.UUID('00000046-39c2-4488-9bb0-00cb43c9ccd4'))]:
            self.media(); self.entry(1, first, last, 'other', guid); self.rebuild()
            self.assertNotEqual(self.inspect()[0], 0)

    def test_unique_exact_boot_name_and_gpt_write_protection(self):
        self.media(); self.entry(1, 500000, 520000, 'boot_b', uuid.UUID(int=2)); self.rebuild()
        self.assertNotEqual(self.inspect()[0], 0)
        self.media(); self.entries[70] = 1; self.rebuild()
        self.assertNotEqual(self.inspect()[0], 0)
        self.media(); self.entries[56:68] = 'boot_a'.encode('utf-16le'); self.rebuild()
        self.assertEqual(self.inspect()[0], ERROR|14)
        self.media(); struct.pack_into('<Q', self.entries, 48, 1 << 60); self.rebuild()
        self.assertEqual(self.inspect()[0], ERROR|8)

    def test_host_proposal_is_compared_to_live_boot_partition(self):
        for field in ('boot_start', 'boot_blocks', 'scratch_start', 'scratch_blocks', 'guard_blocks'):
            self.media(); setattr(self.grant, field, getattr(self.grant, field)+1)
            self.assertNotEqual(self.inspect()[0], 0)
        self.media(); self.grant.guid[0] ^= 1
        self.assertEqual(self.inspect()[0], ERROR|26)

    def test_bounded_workspace_failure_does_no_io(self):
        self.media()
        self.assertEqual(self.inspect(WORK_BYTES-1)[0], ERROR|5)
        self.assertEqual(self.reads, [])


if __name__ == '__main__':
    unittest.main()
