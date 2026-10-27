"""Require the EDK2 Dali USB library to archive the Type-C role coordinator."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class UsbRoleWorkerLibraryTests(unittest.TestCase):
    def test_archive_verifies_role_worker_object_and_exports(self):
        builder = (ROOT / "tools/dali/build_usb_libraries.py").read_text()
        self.assertIn("'DaliUsbRoleWorker'", builder)
        for symbol in (
            "dali_usb_role_worker_init",
            "dali_usb_role_worker_poll",
            "dali_usb_role_worker_stop",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, builder)
        inf = (ROOT.parent / "UniXNU-Dali/drivers/DaliUsb/DaliUsbLib.inf").read_text()
        self.assertIn("DaliUsbRoleWorker.c", inf)


if __name__ == "__main__":
    unittest.main()
