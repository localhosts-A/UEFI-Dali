"""Actual native MMIO/DMA adapter and pinned Mu memory code; no device I/O."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UsbXhciIoTests(unittest.TestCase):
    def test_native_xhci_io(self):
        edk = ROOT / 'Mu_Basecore'
        xhci = edk / 'MdeModulePkg/Bus/Pci/XhciDxe'
        cc = os.environ.get('CC', 'clang')
        with tempfile.TemporaryDirectory(prefix='dali-xhci-io-') as directory:
            base = Path(directory)
            flags = [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DMDEPKG_NDEBUG',
                     '-D_PCD_GET_MODE_16_PcdDelayXhciHCReset=2000',
                     '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                     '-fshort-wchar', '-ffunction-sections', '-fdata-sections', '-Wall', '-Werror',
                     '-I' + str(edk / 'MdePkg/Include'), '-I' + str(edk / 'MdePkg/Include/X64'),
                     '-I' + str(edk / 'MdeModulePkg/Include'), '-I' + str(xhci),
                     '-I' + str(ROOT / 'Platforms/Xiaomi/daliPkg/Include')]
            sources = [ROOT / 'Platforms/Xiaomi/daliPkg/Library/UsbXhciIoLib/UsbXhciIoLib.c',
                       ROOT / 'tools/dali/fixtures/usb_xhci_io_test.c', xhci / 'UsbHcMem.c',
                       xhci / 'XhciReg.c', xhci / 'XhciSched.c']
            objects = []
            for n, source in enumerate(sources):
                obj = base / f'{n}.o'
                # The production adapter and fixture enforce conversion checks;
                # pinned Mu memory code is compiled unchanged with its own ABI.
                subprocess.run(flags + (['-Wextra', '-Wconversion'] if n < 2 else []) +
                               ['-c', str(source), '-o', str(obj)], check=True, timeout=60)
                objects.append(str(obj))
            target = base / 'xhci-io'
            subprocess.run(flags + objects + ['-Wl,--gc-sections', '-o', str(target)], check=True, timeout=60)
            subprocess.run([str(target)], check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
