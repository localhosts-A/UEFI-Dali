"""The opt-in DXE service tees DEBUG SerialPortLib output without Boot Services at log time."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEVICE = ROOT.parent / "UniXNU-Dali"
GENERIC = ROOT.parent / "UniXNU"


class CdcLogDxeTests(unittest.TestCase):
    def test_fv_feature_is_default_off_and_separately_verifiable(self):
        dsc = (ROOT / "Platforms/Xiaomi/daliPkg/dali.dsc").read_text()
        dxe_fv = (ROOT / "Platforms/Xiaomi/daliPkg/Include/DXE.inc").read_text()
        builder = (ROOT / "Platforms/Xiaomi/daliPkg/DeviceBuild.py").read_text()
        verifier = (ROOT / "tools/dali/verify_usb_fv.py").read_text()
        self.assertIn("DEFINE DALI_USB_CDC_LOG = FALSE", dsc)
        self.assertIn("UsbCdcLogDxe.inf", dsc)
        self.assertIn("UsbCdcLogDxe.inf", dxe_fv)
        self.assertIn('os.environ.get("DALI_USB_CDC_LOG") == "true"', builder)
        self.assertIn("--cdc-log", verifier)
        self.assertIn("--cdc-log-only", verifier)

    def test_actual_service_and_serial_library(self):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        self.assertIsNotNone(compiler)
        edk = ROOT / "Mu_Basecore"
        with tempfile.TemporaryDirectory(prefix="dali-cdc-log-dxe-") as directory:
            temp = Path(directory)
            library = temp / "Library"
            library.mkdir()
            (library / "ArmLib.h").write_text("""#pragma once
#include <Uefi.h>
BOOLEAN ArmGetInterruptState(VOID);
VOID ArmDisableInterrupts(VOID);
VOID ArmEnableInterrupts(VOID);
""")
            (library / "Mt6991PlatformLib.h").write_text("""#pragma once
#include <Base.h>
extern UINT8 gConsole[76];
#define DALI_CONSOLE_BASE ((UINTN)gConsole)
#define DALI_CONSOLE_SIZE 76U
""")

            flags = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DUNIXNU_USB_EDK2",
                     "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                     "-fshort-wchar", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
                     "-I" + str(temp), "-I" + str(edk / "MdePkg/Include"),
                     "-I" + str(edk / "MdePkg/Include/X64"), "-I" + str(edk / "ArmPkg/Include"),
                     "-I" + str(ROOT / "Platforms/Xiaomi/daliPkg/Include"),
                     "-I" + str(ROOT / "Silicon/MediaTek/MT6991Pkg/Include"),
                     "-I" + str(GENERIC / "drivers/UsbCore/include"),
                     "-I" + str(DEVICE / "drivers/DaliUsbGadgetLog")]
            sources = [ROOT / "tools/dali/fixtures/usb_cdc_log_dxe_test.c",
                       ROOT / "Platforms/Xiaomi/daliPkg/Drivers/UsbCdcLogDxe/UsbCdcLogDxe.c",
                       ROOT / "Platforms/Xiaomi/daliPkg/Library/RamoopsCdcSerialPortLib/RamoopsCdcSerialPortLib.c",
                       DEVICE / "drivers/DaliUsbGadgetLog/DaliUsbCdcLog.c",
                       GENERIC / "drivers/UsbCore/UsbCore.c",
                       GENERIC / "drivers/UsbCore/UsbMtu3Device.c"]
            target = temp / "cdc-log-dxe"
            command = flags + [str(source) for source in sources] + ["-Wl,--gc-sections", "-o", str(target)]
            subprocess.run(command, check=True, timeout=90)
            subprocess.run([str(target)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
