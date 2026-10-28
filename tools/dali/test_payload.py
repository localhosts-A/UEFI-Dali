import sys
from pathlib import Path
import unittest
import struct


TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from make_payload import (config_with_run_token, config_with_xnu_argument, external_boot_stage_size_mib,
                           payload_volume_size_mib, validate_packed_data_initrd, validate_ram_root,
                           qualification_config)


class PayloadVolumeSizingTests(unittest.TestCase):
    def test_xnu_volume_defaults_to_memory_safe_size(self):
        self.assertEqual(payload_volume_size_mib(None, True), 24)
        self.assertEqual(payload_volume_size_mib(24, True), 24)

    def test_xnu_volume_rejects_size_that_caused_hardware_allocation_failure(self):
        with self.assertRaisesRegex(ValueError, "at most 24 MiB"):
            payload_volume_size_mib(48, True)

    def test_uefi_only_volume_keeps_small_default_and_fv_limit(self):
        self.assertEqual(payload_volume_size_mib(None, False), 1)
        self.assertEqual(payload_volume_size_mib(48, False), 48)
        with self.assertRaisesRegex(ValueError, "64 MiB Dali firmware volume"):
            payload_volume_size_mib(49, False)

    def test_external_stage_scales_for_the_full_kernel_and_keeps_fat_headroom(self):
        self.assertGreaterEqual(external_boot_stage_size_mib(None, 126 * 1024 * 1024), 152)
        self.assertEqual(external_boot_stage_size_mib(160, 126 * 1024 * 1024), 160)
        with self.assertRaisesRegex(ValueError, "headroom"):
            external_boot_stage_size_mib(100, 99 * 1024 * 1024)
        with self.assertRaisesRegex(ValueError, "1..512 MiB"):
            external_boot_stage_size_mib(513, 1)


class PackedDataInitrdTests(unittest.TestCase):
    def test_dbrdisk1_header_and_extent_validation(self):
        valid = struct.pack("<8sIIQQ", b"DBRDISK1", 1, 4096, 4096, 0)
        self.assertEqual(validate_packed_data_initrd(valid), 4096)
        extent = struct.pack("<8sIIQQ", b"DBRDISK1", 1, 4096, 8192, 1)
        extent += struct.pack("<QQ", 0, 4096) + b"D" * 4096
        self.assertEqual(validate_packed_data_initrd(extent), 8192)
        for malformed in (valid[:-1], valid.replace(b"DBRDISK1", b"BADSEED!"),
                          struct.pack("<8sIIQQ", b"DBRDISK1", 1, 4096, 4096, 1) +
                          struct.pack("<QQ", 0, 8192) + b"D" * 8192):
            with self.subTest(length=len(malformed)), self.assertRaises(ValueError):
                validate_packed_data_initrd(malformed)


class RamRootTests(unittest.TestCase):
    def test_ram_root_needs_disabled_watchdog_and_both_zero_handoff_slots(self):
        def node(name, extra=(), children=()):
            values = [(b'name', name.encode() + b'\0'), *extra]
            return struct.pack('<II', len(values), len(children)) + b''.join(
                struct.pack('<32sI', key, len(value)) + value + bytes(-len(value) % 4)
                for key, value in values) + b''.join(children)
        adt = node('device-tree', children=[node('chosen', children=[node('memory-map',
                   [(b'RAMDisk', bytes(16)), (b'TrustCache', bytes(16))])])])
        root = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, 8, 0, 0)
        trust = struct.pack('<3I', 1, 8, 2) + bytes(16) + struct.pack('<I', 1) + bytes(20) + bytes((2, 0, 0, 0))
        config = {'entry': [{'type': 'xnu', 'initrd': 'Payloads/root.img', 'args': 'rd=md0 dali-pid1=1'}]}
        validate_ram_root(config, root, trust, adt, False)
        for inputs in [(config, root, trust, adt, True),
                       (config, root, trust[:-1], adt, False),
                       (config, root, trust, adt[:-1], False),
                       (config, root, trust, adt.replace(b'TrustCache', b'WrongCache'), False),
                       ({'entry': []}, root, trust, adt, False)]:
            with self.subTest(inputs=inputs[-1]), self.assertRaises(ValueError):
                validate_ram_root(*inputs)


class RunTokenTests(unittest.TestCase):
    def test_qualification_forwards_only_one_method_and_rejects_competing_policy(self):
        config = ('[[entry]]\nid = "xnu"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n'
                  'args = "-v dali-earlyboot=1"\n')
        token = 'a' * 32
        for method in ('off', 'psci-cold', 'psci-warm', 'aee'):
            result = qualification_config(config, method, token, False)
            self.assertIn('dali-reset-method=' + method + ' dali-reset-delay=5', result)
        for arguments in (('unknown', token, False, False), ('psci-cold', None, False, False),
                          ('psci-cold', token, True, False), ('psci-cold', token, False, True)):
            with self.assertRaises(ValueError):
                qualification_config(config, *arguments)
        with self.assertRaises(ValueError):
            qualification_config(result, 'psci-cold', token, False)

    def test_auto_recovery_switch_is_forwarded_without_changing_allocator_or_token(self):
        config = ('[[entry]]\nid = "xnu"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n'
                  'args = "-v dali-earlyboot=1 kt_fixed_zones=120"\n')
        for enabled in ('0', '1'):
            with self.subTest(enabled=enabled):
                result = config_with_xnu_argument(config, 'dali-auto-fastboot', enabled)
                result = config_with_run_token(result, '0123456789abcdef0123456789abcdef')
                self.assertIn('kt_fixed_zones=120 dali-auto-fastboot=' + enabled + ' dali-run=', result)
                with self.assertRaises(ValueError):
                    config_with_xnu_argument(result, 'dali-auto-fastboot', enabled)

    def test_token_is_injected_only_into_the_xnu_entry(self):
        token = "0123456789abcdef0123456789abcdef"
        config = ('version = 1\n\n[[entry]]\nid = "hello"\ntype = "efi"\n'
                  'path = "Payloads/HelloPayload.efi"\nargs = "driver var"\n\n'
                  '[[entry]]\nid = "xnu"\ntype = "xnu"\n'
                  'path = "Payloads/kernel.macho"\nargs = "-v dali-earlyboot=1"\n')
        result = config_with_run_token(config, token)
        self.assertIn('args = "-v dali-earlyboot=1 dali-run=' + token + '"', result)
        self.assertIn('args = "driver var"', result)

    def test_invalid_or_duplicate_run_token_fails_closed(self):
        config = ('[[entry]]\nid = "xnu"\ntype = "xnu"\n'
                  'path = "Payloads/kernel.macho"\nargs = "-v dali-earlyboot=1"\n')
        with self.assertRaisesRegex(ValueError, "32 lowercase"):
            config_with_run_token(config, "not-a-token")
        with self.assertRaisesRegex(ValueError, "fresh run token"):
            config_with_run_token(config.replace('"-v dali-earlyboot=1"',
                                                  '"-v dali-earlyboot=1 dali-run=0123456789abcdef0123456789abcdef"'),
                                  "0123456789abcdef0123456789abcdef")


if __name__ == "__main__":
    unittest.main()
