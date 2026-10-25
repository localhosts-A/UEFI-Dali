"""Production DXE context, real EDK2 ABI/libfdt, modeled CPU/GCD services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEVICE = Path(os.environ.get('DALI_DEVICE_ROOT', ROOT.parent / 'UniXNU-Dali'))
GENERIC = Path(os.environ.get('UNIXNU_ROOT', ROOT.parent / 'UniXNU'))


class UsbResourceContextTests(unittest.TestCase):
    def test_production_resource_context(self):
        edk = ROOT / 'Mu_Basecore'
        libfdt = edk / 'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        cc = os.environ.get('CC', 'clang')
        flags = [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        with tempfile.TemporaryDirectory(prefix='dali-usb-context-') as directory:
            base = Path(directory)
            (base / 'Library').mkdir()
            (base / 'Library/Mt6991PlatformLib.h').write_text(
                '#define DALI_FDT_BASE ((UINTN)tree)\n#define DALI_FDT_SIZE sizeof tree\n'
                '#define DALI_PMIF_BASE 0x1c01a000U\n')
            names = {'CheckHeader': 'check_header', 'TotalSize': 'totalsize',
                     'GetProp': 'getprop', 'PathOffset': 'path_offset',
                     'StringListSearch': 'stringlist_search', 'StringListCount': 'stringlist_count',
                     'ParentOffset': 'parent_offset', 'NextNode': 'next_node',
                     'FirstPropertyOffset': 'first_property_offset', 'NextPropertyOffset': 'next_property_offset',
                     '32ToCpu': None}
            # Linux size_t and EDK2 UINTN have equal width but distinct C types
            # on X64. Adapt only the non-public fdt_check_full declaration.
            shim = ('#pragma once\n#define fdt_check_full HostFdtCheckFullDeclaration\n'
                    '#include <libfdt.h>\n#undef fdt_check_full\n'
                    '#define fdt_check_full TestUefiFdtCheckFull\n')
            for name, actual in names.items():
                shim += f'#define Fdt{name} ' + ('fdt32_to_cpu' if actual is None else f'fdt_{actual}') + '\n'
            (base / 'Library/FdtLib.h').write_text(shim)
            bridge = base / 'fdt_bridge.c'
            bridge.write_text('#include <libfdt.h>\n'
                              'int TestUefiFdtCheckFull(const void *fdt, unsigned long long size) {\n'
                              '  return fdt_check_full(fdt, (size_t)size);\n}\n')
            objects = []
            for source in sorted(libfdt.glob('*.c')):
                obj = base / (source.stem + '.o')
                subprocess.run(flags + ['-I', str(libfdt), '-c', str(source), '-o', str(obj)],
                               check=True, timeout=60)
                objects.append(str(obj))
            bridge_obj = base / 'fdt_bridge.o'
            subprocess.run(flags + ['-I', str(libfdt), '-c', str(bridge), '-o', str(bridge_obj)],
                           check=True, timeout=60)
            objects.append(str(bridge_obj))
            target = base / 'context'
            # The portable USB types use the host stdint/size_t definitions here;
            # Boot/DXE service structures still use the actual EDK2 ABI. The
            # firmware build separately exercises UNIXNU_USB_EDK2 on AArch64.
            command = flags + ['-fshort-wchar', '-DMDEPKG_NDEBUG',
                               '-DDALI_I2C_FIXTURE_FILE="' + str(DEVICE / 'tests/fixtures/usb_i2c6_test.c') + '"',
                               '-Wall', '-Wextra', '-Werror', '-Wconversion',
                               '-I' + str(base), '-isystem', str(libfdt),
                               '-I' + str(edk / 'MdePkg/Include'),
                               '-I' + str(edk / 'MdePkg/Include/X64'),
                               '-I' + str(edk / 'UefiCpuPkg/Include'),
                               '-I' + str(ROOT / 'Platforms/Xiaomi/daliPkg/Include'),
                               '-I' + str(DEVICE / 'drivers/DaliUsb/include'),
                               '-I' + str(GENERIC / 'drivers/UsbCore/include'),
                               str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbCalibration.c'),
                               str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbClockResources.c'),
                               str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbVusbResources.c'),
                               str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPlatformLib/UsbI2cResources.c'),
                               str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbPmicLib/UsbPmicLib.c'),
                               str(ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbI2cLib/UsbI2cLib.c'),
                               str(GENERIC / 'drivers/UsbCore/UsbCore.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbPmif.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbVusb.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbI2cPins.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbI2cClocks.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbClockRates.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbI2c.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbGpio.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbNxpRepeater.c'),
                               str(DEVICE / 'drivers/DaliUsb/DaliUsbCalibration.c'),
                               str(ROOT / 'tools/dali/fixtures/usb_resource_context_test.c'),
                               *objects, '-o', str(target)]
            subprocess.run(command, check=True, timeout=60)
            args = [str(target)]
            if os.environ.get('DALI_USB_SAVED_FDT'):
                args.append(os.environ['DALI_USB_SAVED_FDT'])
            subprocess.run(args, check=True, timeout=90)


if __name__ == '__main__':
    unittest.main()
