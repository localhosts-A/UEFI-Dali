"""Execute complete production scratch BlockIo + range/CDB code with real Mu headers."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsScratchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[2]
        pkg = root/'Silicon/MediaTek/MT6991Pkg'
        inc = root/'Mu_Basecore/MdePkg/Include'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-fshort-wchar',
                        '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                        '-include', 'Uefi.h', '-I', str(inc), '-I', str(inc/'AArch64'), '-I', str(pkg/'Include'),
                        '-I', str(pkg/'Drivers/UfsHostDxe'),
                        str(root/'tools/dali/ufs_scratch_harness.c'),
                        str(pkg/'Drivers/UfsHostDxe/UfsPolicy.c'),
                        str(pkg/'Drivers/UfsHostDxe/UfsLayout.c'),
                        '-o', str(Path(cls.temp.name)/'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(Path(cls.temp.name)/'test.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def case(self, mode=1):
        out = (ctypes.c_uint64*40)()
        self.lib.run(mode, out)
        result = list(out)
        self.assertEqual(result[38], result[39], 'teardown must release retained allocations')
        self.assertEqual(result[21], 0, 'requests must satisfy actual lower command policy')
        return result

    def test_disabled_grant_has_no_boot_services_or_device_contact(self):
        r = self.case(0)
        self.assertEqual(r[:3], [19, 0, 0])
        self.assertEqual(r[23], 0)

    def test_real_relative_io_and_sync_use_only_three_exact_commands(self):
        r = self.case()
        self.assertEqual((r[0], r[2]), (0, 1))
        self.assertEqual(r[3:12], [0, 0, 0, 2, 4, 13, 2, 0, 3])
        self.assertEqual(r[12:15], [4096, 255, 0])
        self.assertEqual(r[20], 3)
        self.assertEqual(r[24:27], [0x28, 0x2a, 0x35])
        self.assertEqual(r[28:30], [434432, 434687])
        self.assertEqual(r[32:35], [10, 10, 10])
        self.assertEqual(r[23], 4, 'real reciprocal GPT must be read before publication')

    def test_gpt_failure_media_or_grant_change_prevents_publication_and_writes(self):
        for mode, status, reads in [(33, 10, 2), (34, 18, 3), (35, 13, 2), (38, 13, 3)]:
            r = self.case(mode)
            self.assertEqual(r[0], status)
            self.assertEqual(r[2], 0)
            self.assertEqual(r[20], 0)
            self.assertEqual(r[23], reads)

    def test_inflight_layout_read_cannot_be_stopped_or_reopened(self):
        r = self.case(36)
        self.assertEqual((r[0], r[2], r[20], r[22], r[23]), (13, 0, 0, 6, 3))
        r = self.case(37)
        self.assertEqual((r[0], r[20], r[22]), (13, 0, 0))

    def test_geometry_is_not_a_fixed_four_kib_or_32_bit_lba_assumption(self):
        r = self.case(2)
        self.assertEqual(r[12:15], [512, 2047, 0])
        self.assertEqual(r[28:30], [545536, 547583])
        r = self.case(3)
        self.assertEqual(r[24:27], [0x88, 0x8a, 0x35])
        self.assertEqual(r[28:30], [0x100000000+15872, 0x100000000+16127])
        self.assertEqual(r[32:35], [16, 16, 10])

    def test_expdb_child_has_only_its_explicit_one_mib_window(self):
        r=self.case(40)
        self.assertEqual((r[0],r[2],r[20],r[23]),(0,1,3,4))
        self.assertEqual(r[12:15],[4096,255,0])
        self.assertEqual(r[28:30],[2440,2695])
        self.assertEqual(r[6],2)  # first relative block beyond the scratch window

    def test_authorization_runs_only_after_all_gpt_reads_before_publication(self):
        r=self.case(41)
        self.assertEqual((r[0],r[2],r[20],r[22],r[23]),(0,1,3,4,4))
        self.assertEqual(r[28:30],[2440,2695])
        for mode in (42,43,44,45):
            with self.subTest(mode=mode):
                r=self.case(mode)
                self.assertEqual((r[0],r[2],r[20],r[22],r[23]),(26,0,0,4,4))
        r=self.case(46)
        self.assertEqual((r[0],r[3],r[20]),(0,13,0))
        r=self.case(47)
        self.assertEqual((r[0],r[1],r[2],r[20],r[23]),(15,0,0,0,0))

    def test_any_submitted_error_is_sticky_and_never_replayed(self):
        for mode, first in [(4, 18), (5, 7), (6, 7), (7, 7), (8, 13)]:
            with self.subTest(mode=mode):
                r = self.case(mode)
                self.assertEqual(r[3:6], [first, 7, 7])
                self.assertEqual(r[20], 1)

    def test_success_without_actual_device_sync_completion_is_rejected(self):
        r = self.case(9)
        self.assertEqual(r[3:7], [0, 0, 7, 7])
        self.assertEqual(r[20], 3)
        r = self.case(31)
        self.assertEqual(r[5:7], [7, 7])
        self.assertEqual(r[20], 3)
        r = self.case(32)
        self.assertEqual(r[5], 7)
        self.assertEqual(r[20], 2)  # exhausted counter refuses before sync

    def test_write_timeout_partial_and_warning_are_not_replayed_or_followed_by_sync(self):
        for mode, error in [(24, 18), (25, 7), (27, 7)]:
            with self.subTest(mode=mode):
                r = self.case(mode)
                self.assertEqual(r[3:6], [0, error, 7])
                self.assertEqual(r[20], 2)
                self.assertEqual(r[24:26], [0x28, 0x2a])

    def test_sync_timeout_is_not_replayed_and_blocks_following_requests(self):
        r = self.case(26)
        self.assertEqual(r[3:7], [0, 0, 18, 7])
        self.assertEqual(r[20], 3)

    def test_mutated_identity_or_wp_and_closed_host_never_submit(self):
        for mode, first in [(10, 13), (20, 13), (22, 12), (23, 3)]:
            with self.subTest(mode=mode):
                r = self.case(mode)
                self.assertEqual(r[3], first)
                self.assertEqual(r[20], 0)

    def test_unknown_wp_writable_root_bad_alignment_or_duplicate_lun_never_publish(self):
        for mode, status in [(11, 8), (12, 13), (13, 3), (14, 14), (15, 3), (19, 8)]:
            with self.subTest(mode=mode):
                r = self.case(mode)
                self.assertEqual(r[0], status)
                self.assertEqual(r[2], 0)
                self.assertEqual(r[20], 0)

    def test_allocation_and_install_failure_cleanup_and_idempotent_close(self):
        for mode in [16, 17]:
            r = self.case(mode)
            self.assertEqual(r[0], 9)
            self.assertEqual(r[36:38], [0, 0])

    def test_failed_uninstall_retains_closed_protocol_and_never_uses_parent_again(self):
        r = self.case(18)
        self.assertEqual(r[15:19], [15, 1, 1, 12])
        self.assertEqual(r[36:38], [0, 0])

    def test_reentrant_request_cannot_submit_or_share_sense_buffer(self):
        r = self.case(21)
        self.assertEqual(r[22], 6)
        self.assertEqual(r[20], 3)

    def test_disconnected_parent_or_missing_target_completion_never_reports_success(self):
        r = self.case(28)
        self.assertEqual(r[3:6], [13, 7, 7])
        self.assertEqual(r[20], 0)
        r = self.case(29)
        self.assertEqual(r[3:6], [7, 7, 7])
        self.assertEqual(r[20], 1)

    def test_inflight_unpublish_defers_release_and_invalidates_outer_completion(self):
        r = self.case(30)
        self.assertEqual(r[3:6], [13, 12, 12])
        self.assertEqual(r[20], 1)
        self.assertEqual(r[22], 6)
        self.assertEqual(r[15], 0)  # safe to uninstall after the call returns
