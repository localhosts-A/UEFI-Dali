"""Actual UEFI acquisition/common decoder with pinned real libfdt; no device I/O."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEVICE = Path(os.environ.get('DALI_DEVICE_ROOT', ROOT.parent / 'UniXNU-Dali'))
GENERIC = Path(os.environ.get('UNIXNU_ROOT', ROOT.parent / 'UniXNU'))


class UsbCalibrationTests(unittest.TestCase):
    def test_actual_fdt_calibration(self):
        self.check_fixture('usb_calibration_fdt_test.c', 'production UEFI calibration graph',
                           'saved Dali FDT calibration acquisition')

    def test_actual_fdt_clock_resources(self):
        self.check_fixture('usb_clock_fdt_test.c', 'production UEFI clock resource graph',
                           'saved Dali FDT clock resource acquisition')

    def test_actual_fdt_vusb_resources(self):
        self.check_fixture('usb_vusb_fdt_test.c', 'production UEFI MT6373 VUSB FDT graph',
                           'saved Dali FDT MT6373 VUSB supply acquisition')

    def test_actual_fdt_i2c_resources(self):
        self.check_fixture('usb_i2c_fdt_test.c', 'production UEFI I2C6 pin/reset resource graph',
                           'saved Dali FDT I2C6 pin/reset graph acquisition')

    def check_fixture(self, fixture, marker, saved_marker):
        cc = shutil.which(os.environ.get('CC', 'clang'))
        self.assertIsNotNone(cc)
        libfdt = ROOT / 'Mu_Basecore/MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        self.assertTrue((libfdt / 'fdt.c').is_file())
        with tempfile.TemporaryDirectory(prefix='dali-usb-fdt-cal-') as tmp:
            base = Path(tmp)
            (base / 'Library').mkdir()
            (base / 'Uefi.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void VOID; typedef char CHAR8; typedef uint32_t UINT32;
typedef int32_t INT32; typedef size_t UINTN; typedef uint8_t BOOLEAN;
typedef uint64_t EFI_STATUS; typedef uint16_t UINT16;
#define STATIC static
#define CONST const
#define IN
#define OUT
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_COMPROMISED_DATA 33
#define EFI_ERROR(Value) ((Value) != EFI_SUCCESS)
''')
            names = {'CheckHeader': 'check_header', 'TotalSize': 'totalsize',
                     'GetProp': 'getprop', 'PathOffset': 'path_offset',
                     'StringListSearch': 'stringlist_search', 'StringListCount': 'stringlist_count',
                     'ParentOffset': 'parent_offset', 'NextNode': 'next_node',
                     'FirstPropertyOffset': 'first_property_offset', 'NextPropertyOffset': 'next_property_offset',
                     '32ToCpu': None}
            shim = '#pragma once\n#include <libfdt.h>\n'
            for name, actual in names.items():
                shim += f'#define Fdt{name} ' + ('fdt32_to_cpu' if actual is None else f'fdt_{actual}') + '\n'
            (base / 'Library/FdtLib.h').write_text(shim)
            flags = [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            objects = []
            for source in sorted(libfdt.glob('*.c')):
                obj = base / (source.stem + '.o')
                ran = subprocess.run(flags + ['-I', str(libfdt), '-c', str(source), '-o', str(obj)],
                                     text=True, capture_output=True, timeout=60)
                self.assertEqual(ran.returncode, 0, ran.stderr)
                objects.append(str(obj))
            binary = base / 'calibration'
            command = flags + ['-Wall', '-Wextra', '-Werror', '-Wconversion',
                              '-I', str(base), '-isystem', str(libfdt),
                              '-I', str(ROOT / 'Platforms/Xiaomi/daliPkg/Include'),
                              '-I', str(DEVICE / 'drivers/DaliUsb/include'),
                              '-I', str(GENERIC / 'drivers/UsbCore/include'),
                              str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbCalibration.c'),
                              str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbClockResources.c'),
                              str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbVusbResources.c'),
                              str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbI2cResources.c'),
                              str(DEVICE / 'drivers/DaliUsb/DaliUsbCalibration.c'),
                              str(ROOT / 'tools/dali/fixtures' / fixture),
                              *objects, '-o', str(binary)]
            ran = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            args = [str(binary)]
            saved = os.environ.get('DALI_USB_SAVED_FDT')
            if saved:
                args.append(saved)
            ran = subprocess.run(args, text=True, capture_output=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS: ' + marker, ran.stdout)
            if saved:
                self.assertIn('PASS: ' + saved_marker, ran.stdout)
            print(ran.stdout.strip())


if __name__ == '__main__':
    unittest.main()
