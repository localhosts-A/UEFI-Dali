"""Require the actual EDK2 USB archive to verify portable xHCI endpoint APIs."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class UsbXhciLibraryTests(unittest.TestCase):
    def test_archive_verifies_usb2_noncontrol_endpoint_exports(self):
        builder = (ROOT / "tools/dali/build_usb_libraries.py").read_text()
        for symbol in (
            "usb_xhci_device_configure_endpoint",
            "usb_xhci_device_interrupt",
            "usb_xhci_device_bulk",
            "usb_xhci_device_reset_endpoint",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, builder)


if __name__ == "__main__":
    unittest.main()
