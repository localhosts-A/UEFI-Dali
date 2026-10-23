import sys
from pathlib import Path
import unittest


TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from make_payload import config_with_run_token


class RunTokenTests(unittest.TestCase):
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
