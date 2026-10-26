"""Bind the requested register-probe mode to actual generated BDS build inputs."""
from pathlib import Path
import lzma
import os
import struct
import subprocess
import tempfile
import unittest
from package import (ufs_probe_metadata, ufs_host_off_metadata, ufs_read_only_metadata,
                     validate_diagnostic_payload, PHYSICAL_UFS_MODULES)
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


def combined_firmware(modules):
    first = modules[0]
    output = bytearray(firmware(first[1], guid=first[0]))
    output[72:] = b'\xff' * (len(output) - 72)
    at = 72
    for guid, module in modules:
        single = firmware(module, guid=guid)
        size = int.from_bytes(single[92:95], 'little')
        output[at:at + size] = single[72:72 + size]
        at = (at + size + 7) & ~7
    return bytes(output)


class PhysicalPackagingTests(unittest.TestCase):
    token = 'a1' * 16

    def fixture(self, folder):
        sources = [('daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG', 'Probe', b'none\0'),
                   ('MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG', 'Storage', self.token.encode() + b'\0')]
        for directory, pcd, value in sources:
            auto = folder / directory
            auto.mkdir(parents=True, exist_ok=True)
            (auto / 'AutoGen.h').write_text('#define _PCD_VALUE_PcdDaliUfsProbeEnabled 0U\n'
                                          '#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled 1U\n' +
                                          ('#define _PCD_VALUE_PcdDaliUfsScratchEnabled 0U\n' if pcd == 'Storage' else ''))
            (auto / 'AutoGen.c').write_text(f'const UINT8 _gPcd_FixedAtBuild_PcdDaliUfs{pcd}Token[{len(value)}] = {{' +
                                          ','.join(str(b) for b in value) + '};\n')
        output = folder / 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/OUTPUT'
        output.mkdir(parents=True, exist_ok=True)
        (output / 'static_library_files.lst').write_text(
            '/build/OpensslPkg/Library/BaseCryptLib/BaseCryptLib/OUTPUT/BaseCryptLib.lib\n')
        modules = []
        for name, guid in PHYSICAL_UFS_MODULES.items():
            data = bytearray(128)
            data[:2] = b'MZ'
            struct.pack_into('<I', data, 0x3c, 64)
            data[64:68] = b'PE\0\0'
            struct.pack_into('<H', data, 68, 0xaa64)
            data += name.encode() + b'\0'
            if name == 'DaliUfsHostDxe':
                data += self.token.encode() + b'\0'
            (folder / (name + '.efi')).write_bytes(data)
            modules.append((guid, bytes(data)))
        return combined_firmware(modules)

    def test_explicit_profile_binds_both_consumers_and_all_final_fd_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp); fd = self.fixture(folder)
            probe, host = ufs_read_only_metadata(folder, self.token, fd=fd)
            self.assertFalse(probe['enabled'])
            self.assertTrue(host['enabled'])
            self.assertEqual(host['mode'], 'read-only-blockio')
            self.assertEqual(host['run_token'], self.token)
            self.assertEqual(set(host['modules_verified_in_final_fd']), set(PHYSICAL_UFS_MODULES))
            for key in ('write_authorized', 'media_writes', 'physical_storage_qualified'):
                self.assertFalse(host[key])
            for old in (ufs_probe_metadata, ufs_host_off_metadata):
                with self.assertRaisesRegex(ValueError, 'dedicated candidate verifier'):
                    old(folder, fd=fd)

    def test_wrong_token_either_enable_flag_and_competing_probe_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp); fd = self.fixture(folder)
            for token in ('bad', 'b2' * 16, None):
                with self.assertRaises(ValueError):
                    ufs_read_only_metadata(folder, token, fd=fd)
            for directory in ('daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG',
                              'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG'):
                for old, new in (('ReadOnlyEnabled 1U', 'ReadOnlyEnabled 0U'), ('ProbeEnabled 0U', 'ProbeEnabled 1U')):
                    fd = self.fixture(folder)
                    path = folder / directory / 'AutoGen.h'
                    path.write_text(path.read_text().replace(old, new))
                    with self.assertRaisesRegex(ValueError, 'PCD mismatch'):
                        ufs_read_only_metadata(folder, self.token, fd=fd)
            fd = self.fixture(folder)
            path = folder / 'daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG/AutoGen.c'
            path.write_text(path.read_text().replace('110,111,110,101,0', '110,111,110,102,0'))
            with self.assertRaisesRegex(ValueError, 'token mismatch'):
                ufs_read_only_metadata(folder, self.token, fd=fd)

    def test_null_mixed_duplicate_or_missing_sha_link_library_cannot_be_packaged(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp); fd = self.fixture(folder)
            path = folder / 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/OUTPUT/static_library_files.lst'
            real = path.read_text()
            null = '/build/CryptoPkg/Library/BaseCryptLibNull/BaseCryptLibNull/OUTPUT/BaseCryptLibNull.lib\n'
            for value in ('', null, real + null, real * 2):
                path.write_text(value)
                with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'non-null SHA-256'):
                    ufs_read_only_metadata(folder, self.token, fd=fd)
            path.unlink()
            with self.assertRaises(FileNotFoundError):
                ufs_read_only_metadata(folder, self.token, fd=fd)

    def test_readonly_profile_refuses_compiled_scratch_enable_or_invalid_value(self):
        for value in ('1U','garbage','0','missing','duplicate'):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                folder=Path(tmp);fd=self.fixture(folder)
                path=folder/'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG/AutoGen.h'
                old='#define _PCD_VALUE_PcdDaliUfsScratchEnabled 0U\n'
                replacement='' if value=='missing' else old*2 if value=='duplicate' else old.replace('0U',value)
                path.write_text(path.read_text().replace(old,replacement))
                with self.assertRaisesRegex(ValueError,'scratch write state'):
                    ufs_read_only_metadata(folder,self.token,fd=fd)

    def test_any_missing_stale_wrong_arch_or_duplicate_final_module_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name in PHYSICAL_UFS_MODULES:
                fd = self.fixture(folder)
                (folder / (name + '.efi')).write_bytes(b'MZstale')
                with self.assertRaisesRegex(ValueError, 'differs'):
                    ufs_read_only_metadata(folder, self.token, fd=fd)
            self.fixture(folder)
            path = folder / 'ScsiDisk.efi'; data = bytearray(path.read_bytes()); data[68:70] = b'\x64\x86'; path.write_bytes(data)
            modules = [(guid, (folder / (name + '.efi')).read_bytes()) for name, guid in PHYSICAL_UFS_MODULES.items()]
            with self.assertRaisesRegex(ValueError, 'not ARM64'):
                ufs_read_only_metadata(folder, self.token, fd=combined_firmware(modules))
            self.fixture(folder)
            modules = [(guid, (folder / (name + '.efi')).read_bytes()) for name, guid in PHYSICAL_UFS_MODULES.items()]
            for changed in (modules[:-1], modules + [modules[-1]]):
                with self.assertRaisesRegex(ValueError, 'exactly one'):
                    ufs_read_only_metadata(folder, self.token, fd=combined_firmware(changed))

    def test_diagnostic_payload_refuses_xnu_root_and_automatic_reset(self):
        valid = {'xnu_kernel_sha256': None, 'xnu_ramdisk_sha256': None,
                 'recovery': {'auto_fastboot': False, 'reset_method': None}}
        validate_diagnostic_payload(valid)
        for bad in ({**valid, 'xnu_kernel_sha256': 'a' * 64}, {**valid, 'xnu_ramdisk_sha256': 'b' * 64},
                    {**valid, 'recovery': {'auto_fastboot': True}},
                    {**valid, 'recovery': {'auto_fastboot': False, 'reset_method': 'psci'}}, {}):
            with self.assertRaises(ValueError):
                validate_diagnostic_payload(bad)

    def test_wrapper_refuses_incomplete_opt_in_before_build_tools(self):
        wrapper = Path(__file__).resolve().parents[2] / 'build-dali.sh'
        valid = {'DALI_UFS_PROFILE': 'read-only-blockio', 'DALI_UFS_READ_ONLY': 'true',
                 'DALI_UFS_PROBE': 'false', 'DALI_AUTO_FASTBOOT': 'false', 'DALI_RUN_TOKEN': self.token}
        for change in ({'DALI_UFS_PROFILE': 'off'}, {'DALI_UFS_READ_ONLY': 'false'},
                       {'DALI_UFS_PROBE': 'true'}, {'DALI_AUTO_FASTBOOT': 'true'},
                       {'DALI_RESET_METHOD': 'psci'}, {'DALI_RUN_TOKEN': 'bad'}):
            with tempfile.TemporaryDirectory() as tmp:
                env = {k: v for k, v in os.environ.items() if not k.startswith('DALI_')}
                env.update(valid); env.update(change)
                run = subprocess.run(['bash', str(wrapper), tmp, tmp, tmp], env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 2, run.stderr)
                self.assertNotIn('Initialize', run.stderr)


if __name__ == '__main__':
    unittest.main()
