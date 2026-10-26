"""Bounded fixtures for the combined firmware/source-XNU UFS profile."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import make_payload
from firmware_volume import extract_module

TOKEN = "0123456789abcdef" * 2
LIVE_FDT = b"\xd0\x0d\xfe\xed" + bytes(92)


def apple_node(name, props=(), children=()):
    values = [("name", name.encode() + b"\0"), *props]
    data = struct.pack("<II", len(values), len(children))
    for key, value in values:
        data += struct.pack("<32sI", key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return data + b"".join(children)


def valid_adt():
    resources = struct.pack("<16Q", *make_payload.XNU_UFS_RESOURCE_WORDS) + hashlib.sha256(LIVE_FDT).digest()
    nub = apple_node("dali-ufs", [("compatible", b"localhosts,dali-ufs\0"), ("resources-v1", resources)])
    return apple_node("device-tree", children=[apple_node("arm-io", children=[nub])])


def valid_config(token=TOKEN):
    return {"entry": [{"id": "xnu", "type": "xnu", "path": "Payloads/kernel.macho",
                       "args": f"-v dali-earlyboot=1 dali-auto-fastboot=0 dali-ufs-readonly=1 dali-ufs-stop-after-diagnostics=1 dali-run={token} kt_fixed_zones=120"}]}


def single_raw_fv(guid, payload, file_type=0x02, large=False):
    section = (len(payload) + 4).to_bytes(3, "little") + b"\x19" + payload
    header_size = 32 if large else 24
    file_size = header_size + len(section)
    ffs = bytearray(header_size)
    ffs[:16] = guid
    ffs[18] = file_type
    if large:
        ffs[19] = 0x01
        ffs[24:32] = file_size.to_bytes(8, "little")
    else:
        ffs[20:23] = file_size.to_bytes(3, "little")
    ffs[23] = 0x08
    ffs += section
    volume_size = 56 + len(ffs)
    header = bytearray(56)
    struct.pack_into("<Q", header, 32, volume_size)
    header[40:44] = b"_FVH"
    struct.pack_into("<H", header, 48, 56)
    words = struct.unpack("<28H", header)
    struct.pack_into("<H", header, 50, (-sum(words)) & 0xFFFF)
    return bytes(header + ffs)


class XnuUfsPackageTests(unittest.TestCase):
    def test_live_fdt_hash_and_fixed_resource_node_bind(self):
        adt = valid_adt()
        record = make_payload.validate_xnu_ufs_readonly(valid_config(), adt, LIVE_FDT, TOKEN, False, None, False)
        self.assertEqual(record[-32:], hashlib.sha256(LIVE_FDT).digest())
        for bad_adt, fdt in ((adt[:-1], LIVE_FDT), (adt, LIVE_FDT + b"x"),
                             (adt.replace(hashlib.sha256(LIVE_FDT).digest(), bytes(32)), LIVE_FDT),
                             (adt.replace(struct.pack("<Q", 0x16810000), struct.pack("<Q", 0x16811000), 1), LIVE_FDT)):
            with self.subTest():
                with self.assertRaises(ValueError):
                    make_payload.validate_xnu_ufs_readonly(valid_config(), bad_adt, fdt, TOKEN, False, None, False)

    def test_profile_requires_one_matching_entry_manual_recovery_and_no_reset_or_pid1(self):
        adt = valid_adt()
        cases = [
            (valid_config("f" * 32), TOKEN, False, None, False),
            (valid_config(), "bad", False, None, False),
            (valid_config(), TOKEN, True, None, False),
            (valid_config(), TOKEN, False, "psci-warm", False),
            (valid_config(), TOKEN, False, None, True),
        ]
        for config, token, auto, reset, ramdisk in cases:
            with self.subTest(config=config, token=token, auto=auto, reset=reset, ramdisk=ramdisk):
                with self.assertRaises(ValueError):
                    make_payload.validate_xnu_ufs_readonly(config, adt, LIVE_FDT, token, auto, reset, ramdisk)
        for word in ("dali-earlyboot=0", "dali-ufs-readonly=2", "dali-pid1=1", "dali-reset-method=off",
                     "dali-ufs-scratch-write=1",
                     "dali-ufs-stop-after-diagnostics=0", "dali-ufs-stop-after-diagnostics=1"):
            cfg = valid_config(); cfg["entry"][0]["args"] += " " + word
            with self.subTest(word=word), self.assertRaises(ValueError):
                make_payload.validate_xnu_ufs_readonly(cfg, adt, LIVE_FDT, TOKEN, False, None, False)
        cfg=valid_config();cfg['entry'][0]['args']=cfg['entry'][0]['args'].replace('dali-ufs-stop-after-diagnostics=1','')
        with self.assertRaises(ValueError):
            make_payload.validate_xnu_ufs_readonly(cfg, adt, LIVE_FDT, TOKEN, False, None, False)

    def valid_bundle(self, *, recovery_config=None, resource_hash=None):
        config = ('[[entry]]\nid = "xnu"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n'
                  f'args = "-v dali-earlyboot=1 dali-auto-fastboot=0 dali-ufs-readonly=1 dali-ufs-stop-after-diagnostics=1 dali-run={TOKEN}"\n').encode()
        recovery_config = recovery_config or (f'version = 1\nauto_fastboot = false\nwatchdog_timeout_seconds = 31\nrun_token = {TOKEN}\n').encode()
        adt, kernel = valid_adt(), b'compiled source kernel'
        resources = make_payload.validate_xnu_ufs_readonly(valid_config(), adt, LIVE_FDT, TOKEN, False, None, False)
        files = {
            'EFI/UniXNU/unixnu.toml': config,
            'EFI/UniXNU/dali-boot.conf': recovery_config,
            'EFI/UniXNU/Payloads/kernel.macho': kernel,
            'EFI/UniXNU/Payloads/kernel.macho.adt': adt,
        }
        payload = {
            'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                      for name, data in files.items()},
            'xnu_kernel_sha256': hashlib.sha256(kernel).hexdigest(),
            'xnu_adt_sha256': hashlib.sha256(adt).hexdigest(),
            'xnu_ramdisk_sha256': None, 'xnu_trust_cache_sha256': None,
            'physical_storage': False, 'readonly': True, 'runtime_trust_cache_provider': False,
            'recovery': {'auto_fastboot': False, 'watchdog_timeout_seconds': 31, 'run_token': TOKEN},
            'xnu_ufs_read_only': {'enabled': True, 'run_token': TOKEN, 'write_authorized': False,
                'physical_ufs_qualified': False,
                'resources_sha256': resource_hash or hashlib.sha256(resources).hexdigest(),
                'resources_fdt_sha256': hashlib.sha256(LIVE_FDT).hexdigest(),
                'live_fdt_sha256': hashlib.sha256(LIVE_FDT).hexdigest()},
        }
        return payload, adt, kernel, config, recovery_config

    def test_actual_fat_members_recovery_and_resources_are_bound_to_manifest(self):
        payload, adt, kernel, config, recovery = self.valid_bundle()
        resources = make_payload.validate_xnu_ufs_bundle(payload, TOKEN, adt, LIVE_FDT, config, recovery, kernel)
        self.assertEqual(payload['xnu_ufs_read_only']['resources_sha256'], hashlib.sha256(resources).hexdigest())
        for bad_recovery, bad_resource in (
            (recovery.replace(b'auto_fastboot = false', b'auto_fastboot = true'), None),
            (recovery, 'f' * 64),
        ):
            bad, adt, kernel, config, _ = self.valid_bundle(recovery_config=bad_recovery, resource_hash=bad_resource)
            with self.subTest(bad_recovery=bad_recovery, bad_resource=bad_resource), self.assertRaises(ValueError):
                make_payload.validate_xnu_ufs_bundle(bad, TOKEN, adt, LIVE_FDT, config, bad_recovery, kernel)

    def test_final_volume_extractor_requires_the_selected_freeform_raw_section(self):
        guid = uuid.UUID("073e16da-8776-4fba-96c4-35bbe06a41df").bytes_le
        data = b"DALI FAT payload bytes" * 3
        fd = single_raw_fv(guid, data)
        self.assertEqual(extract_module(fd, guid, "Dali payload", section_type=0x19), data)
        large_fd = single_raw_fv(guid, data, large=True)
        self.assertEqual(extract_module(large_fd, guid, "Dali payload", section_type=0x19,
                                        expected_file_type=0x02), data)
        bad_large = bytearray(large_fd); bad_large[76:79] = b"\x01\x00\x00"
        with self.assertRaisesRegex(ValueError, "invalid extended firmware file"):
            extract_module(bytes(bad_large), guid, "Dali payload", section_type=0x19,
                           expected_file_type=0x02)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            extract_module(fd, uuid.uuid4().bytes_le, "other payload", section_type=0x19)
        driver_file = single_raw_fv(guid, data, file_type=0x07)
        with self.assertRaisesRegex(ValueError, "wrong FFS file type"):
            extract_module(driver_file, guid, "Dali payload", section_type=0x19, expected_file_type=0x02)
        broken = bytearray(fd); broken[80:83] = b"\xff\xff\x7f"
        with self.assertRaisesRegex(ValueError, "section exceeds"):
            extract_module(bytes(broken), guid, "Dali payload", section_type=0x19)


if __name__ == "__main__":
    unittest.main()
