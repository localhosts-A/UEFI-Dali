"""Exercise the firmware's actual C sampling policy with synthetic GPIO times."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class KeypadTimingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("native C compiler required")
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        header = Path(__file__).resolve().parents[2] / "Platforms/Xiaomi/daliPkg/Library/KeypadDeviceLib/KeypadTiming.h"
        (base / "policy.c").write_text('''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef bool BOOLEAN;
#define STATIC static
#define TRUE true
#define FALSE false
#include "KeypadTiming.h"
static DALI_KEY_STATE state;
void reset(void) { state = (DALI_KEY_STATE){0}; }
size_t sample(int pressed, uint64_t now) { return DaliKeySample(&state, pressed != 0, now); }
size_t press_sample(int pressed, uint64_t now) { return DaliKeyPressSample(&state, pressed != 0, now); }
''')
        subprocess.run([compiler, "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(header.parent), str(base / "policy.c"), "-o", str(base / "policy.so")], check=True)
        cls.policy = ctypes.CDLL(str(base / "policy.so"))
        cls.policy.sample.argtypes = [ctypes.c_int, ctypes.c_uint64]
        cls.policy.sample.restype = ctypes.c_size_t
        cls.policy.press_sample.argtypes = [ctypes.c_int, ctypes.c_uint64]
        cls.policy.press_sample.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.policy.reset()

    def sample(self, pressed, ms):
        return self.policy.sample(pressed, ms * 1000000)

    def test_repeated_short_taps_never_become_enter(self):
        for start in range(0, 2000, 200):
            self.assertEqual(self.sample(True, start), 0)
            self.assertEqual(self.sample(True, start + 25), 0)
            self.assertEqual(self.sample(False, start + 80), 0)
            self.assertEqual(self.sample(False, start + 105), 1)

    def test_long_hold_emits_one_enter_and_no_release_up(self):
        self.assertEqual(self.sample(True, 0), 0)
        self.assertEqual(self.sample(True, 25), 0)
        self.assertEqual(self.sample(True, 699), 0)
        self.assertEqual(self.sample(True, 700), 2)
        self.assertEqual(self.sample(True, 1000), 0)
        self.assertEqual(self.sample(False, 1100), 0)
        self.assertEqual(self.sample(False, 1125), 0)

    def test_delayed_sample_uses_elapsed_time(self):
        self.sample(True, 0)
        self.sample(True, 25)
        self.assertEqual(self.sample(True, 900), 2)

    def test_pending_release_after_gap_cannot_emit_enter(self):
        self.sample(True, 0)
        self.sample(True, 25)
        self.assertEqual(self.sample(False, 3000), 0)
        self.assertEqual(self.sample(False, 3025), 1)

    def test_bounce_is_not_a_key(self):
        self.sample(True, 0)
        self.sample(False, 10)
        self.assertEqual(self.sample(False, 40), 0)

    def test_pmic_press_is_debounced_without_hold_repeat(self):
        press = lambda down, ms: self.policy.press_sample(down, ms * 1000000)
        self.assertEqual(press(True, 0), 0)
        self.assertEqual(press(True, 20), 1)
        self.assertEqual(press(True, 1000), 0)
        self.assertEqual(press(False, 1100), 0)
        self.assertEqual(press(False, 1120), 0)
        self.assertEqual(press(True, 1200), 0)
        self.assertEqual(press(True, 1220), 1)

    def test_pmic_bounce_does_not_press_or_rearm(self):
        press = lambda down, ms: self.policy.press_sample(down, ms * 1000000)
        self.assertEqual(press(True, 0), 0)
        self.assertEqual(press(False, 10), 0)
        self.assertEqual(press(False, 30), 0)
        self.assertEqual(press(True, 40), 0)
        self.assertEqual(press(True, 60), 1)
        self.assertEqual(press(False, 70), 0)
        self.assertEqual(press(True, 80), 0)
        self.assertEqual(press(True, 120), 0)


if __name__ == "__main__":
    unittest.main()
