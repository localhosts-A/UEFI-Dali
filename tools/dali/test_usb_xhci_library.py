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

    def test_archive_verifies_xhci_usb_host_adapter_exports(self):
        builder = (ROOT / "tools/dali/build_usb_libraries.py").read_text()
        for symbol in (
            "usb_xhci_host_adapter_init",
            "usb_xhci_host_adapter_get_ops",
            "usb_xhci_host_adapter_reset_root_port",
            "usb_xhci_host_adapter_next_event",
            "usb_xhci_host_adapter_release_stopped",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, builder)
        self.assertIn("'UsbXhciHost'", builder)
        inf = (ROOT.parent / "UniXNU/drivers/UsbCore/UsbCoreLib.inf").read_text()
        self.assertIn("UsbXhciHost.c", inf)


if __name__ == "__main__":
    unittest.main()
