import sys
from pathlib import Path
import unittest


TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from make_payload import config_with_run_token, config_with_xnu_argument, payload_volume_size_mib


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


class RunTokenTests(unittest.TestCase):
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
