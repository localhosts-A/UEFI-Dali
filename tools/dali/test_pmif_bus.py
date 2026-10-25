"""Run actual DXE PMIF service and keypad C with modeled UEFI/MMIO, no device."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "Platforms/Xiaomi/daliPkg"


class PmicBusTests(unittest.TestCase):
    def test_production_bus_and_keypad(self):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="dali-pmif-bus-") as tmp:
            base = Path(tmp)
            # The fixture defines the minimal ABI/model before including actual
            # production source; these empty headers only satisfy include names.
            for name in ("Uefi.h", "Guid/EventGroup.h", "Library/SynchronizationLib.h",
                         "Library/UefiBootServicesTableLib.h", "Library/BaseLib.h",
                         "Library/DebugLib.h", "Library/FdtLib.h", "Library/IoLib.h",
                         "Library/Mt6991PlatformLib.h", "Library/TimerLib.h"):
                header = base / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text("/* Test-only ABI seam. */\n")
            binary = base / "pmif-bus-test"
            command = [compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-Wconversion", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-I", str(base), "-I", str(PACKAGE / "Include"),
                       "-I", str(PACKAGE / "Drivers/PmicBusDxe"),
                       "-I", str(PACKAGE / "Library/KeypadDeviceLib"),
                       str(ROOT / "tools/dali/fixtures/pmif_bus_test.c"), "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("PASS: production DXE shared PMIF", ran.stdout)
            print(ran.stdout.strip())


if __name__ == "__main__":
    unittest.main()
