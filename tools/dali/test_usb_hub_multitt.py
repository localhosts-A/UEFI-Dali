"""Compile the actual Mu USB hub multi-TT selector with a USB IO model."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class UsbHubMultiTtTests(unittest.TestCase):
    def test_actual_source_selector(self):
        edk = ROOT / 'Mu_Basecore'
        bus = edk / 'MdeModulePkg/Bus/Usb/UsbBusDxe'
        cc = os.environ.get('CC', 'clang')
        with tempfile.TemporaryDirectory(prefix='dali-usb-hub-multitt-') as directory:
            base = Path(directory)
            flags = [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                     '-fshort-wchar', '-ffunction-sections', '-fdata-sections',
                     '-Wall', '-Werror',
                     '-I' + str(edk / 'MdePkg/Include'),
                     '-I' + str(edk / 'MdePkg/Include/X64'),
                     '-I' + str(edk / 'MdeModulePkg/Include'),
                     '-I' + str(bus)]
            sources = [ROOT / 'tools/dali/fixtures/usb_hub_multitt_test.c',
                       bus / 'UsbHubMultiTt.c']
            objects = []
            for index, source in enumerate(sources):
                obj = base / f'{index}.o'
                subprocess.run(flags + ['-c', str(source), '-o', str(obj)],
                               check=True, timeout=60)
                objects.append(str(obj))
            target = base / 'usb-hub-multitt'
            subprocess.run(flags + objects + ['-Wl,--gc-sections', '-o', str(target)],
                           check=True, timeout=60)
            subprocess.run([str(target)], check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
