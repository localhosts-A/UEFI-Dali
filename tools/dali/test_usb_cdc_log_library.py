"""The CDC log adapter archive is separately built and only opt-in for the Dali FV."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERIC = ROOT.parent / "UniXNU"
DEVICE = ROOT.parent / "UniXNU-Dali"


class CdcLogLibraryTests(unittest.TestCase):
    def test_edk2_archive_compiles_the_adapter_without_installing_it(self):
        libraries = (ROOT / "Platforms/Xiaomi/daliPkg/UsbLibraries.dsc").read_text()
        builder = (ROOT / "tools/dali/build_usb_libraries.py").read_text()
        firmware = (ROOT / "Platforms/Xiaomi/daliPkg/dali.dsc").read_text()
        inf = (DEVICE / "drivers/DaliUsbGadgetLog/DaliUsbCdcLogLib.inf").read_text()
        adapter = (DEVICE / "drivers/DaliUsbGadgetLog/DaliUsbCdcLog.c").read_text()
        core = (GENERIC / "drivers/UsbCore/include/UsbMtu3Device.h").read_text()

        self.assertIn("DaliUsbCdcLogLib|DaliUsbGadgetLog/DaliUsbCdcLogLib.inf", libraries)
        self.assertIn("DaliUsbGadgetLog/DaliUsbCdcLogLib.inf", libraries)
        self.assertIn("LIBRARY_CLASS = DaliUsbCdcLogLib", inf)
        self.assertIn("DaliUsbCdcLog.c", inf)
        self.assertIn("dali_usb_cdc_log_write", builder)
        self.assertIn("--undefined-only", builder)
        self.assertIn("usb_mtu3_device_log", adapter)
        self.assertIn("size_t usb_mtu3_device_log", core)
        self.assertIn("DEFINE DALI_USB_CDC_LOG = FALSE", firmware)
        self.assertIn("DaliUsbCdcLogLib|DaliUsbGadgetLog/DaliUsbCdcLogLib.inf", firmware)
        self.assertIn("UsbCdcLogDxe.inf", firmware)


if __name__ == "__main__":
    unittest.main()
