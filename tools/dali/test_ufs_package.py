"""Bind the requested register-probe mode to actual generated BDS build inputs."""
from pathlib import Path
import lzma
import struct
import tempfile
import unittest
from package import ufs_probe_metadata, ufs_host_off_metadata
from firmware_volume import BDS_GUID, LZMA_GUID, UFS_HOST_GUID, extract_bds


def firmware(module, nested=False, duplicate=False, guid=BDS_GUID):
    def section(kind, body):
        return (len(body) + 4).to_bytes(3, 'little') + bytes([kind]) + body

    def volume(body, guid=BDS_GUID):
        result = bytearray(b'\xff' * 4096)
        result[:72] = bytes(72)
        struct.pack_into('<Q', result, 32, len(result))
        result[40:44] = b'_FVH'
        struct.pack_into('<IH', result, 44, 0x800, 72)
        result[55] = 2
        struct.pack_into('<II', result, 56, 1, len(result))
        checksum = -sum(struct.unpack('<36H', result[:72])) & 0xffff
        struct.pack_into('<H', result, 50, checksum)
        fh = bytearray(24)
        fh[:16] = guid
        fh[18] = 7
        fh[20:23] = (len(body) + 24).to_bytes(3, 'little')
        fh[23] = 0xf8
        file = bytes(fh) + body
        result[72:72 + len(file)] = file
        if duplicate:
            at = (72 + len(file) + 7) & ~7
            result[at:at + len(file)] = file
        return bytes(result)

    inner = volume(section(0x10, module), guid)
    if not nested:
        return inner
    packed = lzma.compress(section(0x17, inner), format=lzma.FORMAT_ALONE)
    guided = LZMA_GUID + struct.pack('<HH', 24, 1) + packed
    return volume(section(2, guided), bytes(16))


class ProbePackagingTests(unittest.TestCase):
    def fixture(self, folder, token=None, enabled=None):
        auto = folder / 'daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG'
        auto.mkdir(parents=True, exist_ok=True)
        flag = (token is not None) if enabled is None else enabled
        (auto / 'AutoGen.h').write_text(f'#define _PCD_VALUE_PcdDaliUfsProbeEnabled  {int(flag)}U\n'
                                      '#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled  0U\n')
        data = (token or 'none').encode() + b'\0'
        (auto / 'AutoGen.c').write_text(f'const UINT8 _gPcd_FixedAtBuild_PcdDaliUfsProbeToken[{len(data)}] = {{' +
                                        ', '.join(str(byte) for byte in data) + ' };\n')
        module = b'MZcompiled-module-fixture' + data
        (folder / 'DaliDiagnosticBds.efi').write_bytes(module)
        return firmware(module, nested=True)

    def test_off_and_bound_probe_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            fd = self.fixture(folder)
            self.assertFalse(ufs_probe_metadata(folder, fd=fd)['enabled'])
            token = 'a1' * 16
            fd = self.fixture(folder, token)
            result = ufs_probe_metadata(folder, token, fd=fd)
            self.assertEqual(result['run_token'], token)
            self.assertTrue(result['bds_verified_in_final_fd'])
            for key in ('controller_commands', 'mmio_writes', 'physical_storage_qualified'):
                self.assertFalse(result[key])

    def test_stale_enable_token_and_module_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            token = 'a1' * 16
            fd = self.fixture(folder, token)
            for requested in (None, 'b2' * 16, 'bad'):
                with self.subTest(requested=requested), self.assertRaises(ValueError):
                    ufs_probe_metadata(folder, requested, fd=fd)
            fd = self.fixture(folder, token, enabled=False)
            with self.assertRaises(ValueError):
                ufs_probe_metadata(folder, token, fd=fd)
            fd = self.fixture(folder, token)
            (folder / 'DaliDiagnosticBds.efi').write_bytes(b'stale module')
            with self.assertRaisesRegex(ValueError, 'final FD BDS'):
                ufs_probe_metadata(folder, token, fd=fd)

    def test_actual_fd_is_required_not_just_external_build_products(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            token = 'a1' * 16
            self.fixture(folder, token)
            with self.assertRaisesRegex(ValueError, 'final FD BDS'):
                ufs_probe_metadata(folder, token, fd=firmware(b'MZother-module', nested=True))

    def test_register_probe_packager_refuses_physical_host_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            fd = self.fixture(folder)
            header = folder / 'daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG/AutoGen.h'
            header.write_text(header.read_text().replace('ReadOnlyEnabled  0U', 'ReadOnlyEnabled  1U'))
            with self.assertRaisesRegex(ValueError, 'dedicated candidate verifier'):
                ufs_probe_metadata(folder, fd=fd)

    def test_disabled_host_must_match_actual_fd_pcd_and_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            auto = folder / 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG'
            auto.mkdir(parents=True)
            header = auto / 'AutoGen.h'
            header.write_text('#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled  0U\n')
            source = auto / 'AutoGen.c'
            source.write_text('const UINT8 _gPcd_FixedAtBuild_PcdDaliUfsStorageToken[5] = {110,111,110,101,0};\n')
            module = b'MZdisabled-host'
            (folder / 'DaliUfsHostDxe.efi').write_bytes(module)
            fd = firmware(module, nested=True, guid=UFS_HOST_GUID)
            self.assertFalse(ufs_host_off_metadata(folder, fd=fd)['enabled'])
            with self.assertRaisesRegex(ValueError, 'final FD UFS host'):
                ufs_host_off_metadata(folder, fd=firmware(b'MZstale-host', guid=UFS_HOST_GUID))
            header.write_text('#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled  1U\n')
            with self.assertRaisesRegex(ValueError, 'dedicated candidate verifier'):
                ufs_host_off_metadata(folder, fd=fd)
            header.write_text('#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled  0U\n')
            source.write_text(source.read_text().replace('[5]', '[6]'))
            with self.assertRaisesRegex(ValueError, 'token differs'):
                ufs_host_off_metadata(folder, fd=fd)

    def test_volume_bounds_checksum_and_unique_target(self):
        module = b'MZtest'
        self.assertEqual(extract_bds(firmware(module)), module)
        self.assertEqual(extract_bds(firmware(module, nested=True)), module)
        corrupt = bytearray(firmware(module))
        corrupt[56] ^= 1
        for data in (firmware(module)[:100], bytes(corrupt), firmware(module, duplicate=True)):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                extract_bds(data)


if __name__ == '__main__':
    unittest.main()
