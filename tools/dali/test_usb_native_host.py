"""Actual native host lifecycle + patched Mu engine, with register/DMA models."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeHostTests(unittest.TestCase):
    def test_native_host(self):
        edk = ROOT / 'Mu_Basecore'
        cc = os.environ.get('CC', 'clang')
        with tempfile.TemporaryDirectory(prefix='dali-native-host-') as directory:
            base = Path(directory)
            flags = [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                     '-D_PCD_GET_MODE_16_PcdDelayXhciHCReset=2000',
                     '-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=1000000',
                     '-D_PCD_GET_MODE_BOOL_PcdVerifyNodeInList=1',
                     '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                     '-fshort-wchar', '-ffunction-sections', '-fdata-sections', '-Wall', '-Werror',
                     '-I' + str(edk / 'MdePkg/Include'), '-I' + str(edk / 'MdePkg/Include/X64'),
                     '-I' + str(edk / 'MdeModulePkg/Include'),
                     '-I' + str(edk / 'MdeModulePkg/Bus/Pci/XhciDxe'),
                     '-I' + str(ROOT / 'Platforms/Xiaomi/daliPkg/Include'),
                     '-I' + str(ROOT.parent / 'UniXNU/package/UniXNUPkg/Include')]
            library = ROOT / 'Platforms/Xiaomi/daliPkg/Library'
            sources = [ROOT / 'tools/dali/fixtures/usb_native_host_test.c',
                       library / 'UsbNativeHostLib/UsbNativeHostLib.c',
                       library / 'UsbXhciIoLib/UsbXhciIoLib.c']
            sources += [library / ('UsbNativeHostLib/' + name + '.c') for name in
                        ('MuSchedule', 'MuRegister', 'MuMemory')]
            sources += [edk / 'MdePkg/Library/BaseLib/LinkedList.c']
            objects = []
            for n, source in enumerate(sources):
                obj = base / f'{n}.o'
                subprocess.run(flags + ['-c', str(source), '-o', str(obj)], check=True, timeout=60)
                objects.append(str(obj))
            target = base / 'native-host'
            subprocess.run(flags + objects + ['-Wl,--gc-sections', '-o', str(target)], check=True, timeout=60)
            subprocess.run([str(target)], check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
