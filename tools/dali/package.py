#!/usr/bin/env python3
"""Package dali UniXNU UEFI using the verified boot-v4 / legacy-LZ4 format."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import sys
import tomllib
import uuid

from fdt import Fdt
from firmware_volume import extract_bds, extract_module, BDS_GUID, UFS_HOST_GUID
from loader_binding import loader_measurement_metadata

PHYSICAL_UFS_MODULES = {
    'DaliDiagnosticBds': BDS_GUID, 'DaliUfsHostDxe': UFS_HOST_GUID,
    'UfsPassThruDxe': uuid.UUID('e7f1dff9-dab6-498a-9adf-57f344eddf57').bytes_le,
    'ScsiBus': uuid.UUID('0167ccc4-d0f7-4f21-a3ef-9e64b7cdce8b').bytes_le,
    'ScsiDisk': uuid.UUID('0a66e322-3740-4cce-ad62-bd172cecca35').bytes_le,
    'PartitionDxe': uuid.UUID('1fa1f39e-feff-4aae-bd7b-38a070a3b609').bytes_le,
}
XNU_UFS_PAYLOAD_GUID = uuid.UUID('073e16da-8776-4fba-96c4-35bbe06a41df').bytes_le
BOOT_STAGE_FILE_GUID = uuid.UUID('2a20dc84-3c0b-4750-95f9-431436d6ad82').bytes_le
BOOT_STAGE_DESCRIPTOR = struct.Struct('<8sIIQQQQQQII32s32s32s')


def _probe_record(module, source, token):
    return {"enabled": token is not None, "run_token": token,
            "mode": "read-only-registers" if token else "off",
            "controller_commands": False, "mmio_writes": False,
            "physical_storage_qualified": False,
            "bds_sha256": hashlib.sha256(module).hexdigest(),
            "bds_verified_in_final_fd": True,
            "bds_autogen_sha256": hashlib.sha256(source.encode()).hexdigest()}


def ufs_probe_metadata(build, token=None, *, fd):
    """Refuse stale enabled/token build products before packaging a candidate."""
    if token is not None and not re.fullmatch(r"[0-9a-f]{32}", token):
        raise ValueError("invalid UFS probe token")
    auto = build / "daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG"
    header = (auto / "AutoGen.h").read_text()
    source = (auto / "AutoGen.c").read_text()
    physical = re.search(r"^#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled\s+0U\s*$", header, re.M)
    if not physical:
        raise ValueError("physical UFS host requires its dedicated candidate verifier/runner")
    flag = re.search(r"^#define _PCD_VALUE_PcdDaliUfsProbeEnabled\s+([01])U\s*$", header, re.M)
    value = re.search(r"_gPcd_FixedAtBuild_PcdDaliUfsProbeToken\[(\d+)\]\s*=\s*\{([^}]+)\}", source)
    if not flag or not value or int(flag[1]) != int(token is not None):
        raise ValueError("compiled UFS probe enable state differs from packaging request")
    raw = bytes(int(piece.strip(), 0) for piece in value[2].split(',') if piece.strip())
    expected = (token or "none").encode() + b"\0"
    if len(raw) != int(value[1]) or raw != expected:
        raise ValueError("compiled UFS probe token differs from packaging request")
    module = (build / "DaliDiagnosticBds.efi").read_bytes()
    if extract_bds(fd) != module:
        raise ValueError("final FD BDS differs from validated compiled module")
    if token is not None and expected not in module:
        raise ValueError("probe token missing from actual compiled BDS module")
    return _probe_record(module, source, token)


def ufs_host_off_metadata(build, *, fd):
    """The register-probe packaging path cannot qualify the new physical host."""
    auto = build / "MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG"
    header = (auto / "AutoGen.h").read_text()
    source = (auto / "AutoGen.c").read_text()
    if not re.search(r"^#define _PCD_VALUE_PcdDaliUfsReadOnlyEnabled\s+0U\s*$", header, re.M):
        raise ValueError("physical UFS host requires its dedicated candidate verifier/runner")
    value = re.search(r"_gPcd_FixedAtBuild_PcdDaliUfsStorageToken\[(\d+)\]\s*=\s*\{([^}]+)\}", source)
    if not value or int(value[1]) != 5 or bytes(int(v.strip(), 0) for v in value[2].split(',') if v.strip()) != b"none\0":
        raise ValueError("compiled disabled UFS host token differs")
    module = (build / "DaliUfsHostDxe.efi").read_bytes()
    if extract_module(fd, UFS_HOST_GUID, 'UFS host') != module:
        raise ValueError("final FD UFS host differs from validated compiled module")
    return {"enabled": False, "mode": "off", "physical_storage_qualified": False,
            "host_sha256": hashlib.sha256(module).hexdigest(), "host_verified_in_final_fd": True,
            "host_autogen_sha256": hashlib.sha256(source.encode()).hexdigest()}


def ufs_read_only_metadata(build, token, *, fd):
    """Explicit physical profile: both PCD consumers and every storage module."""
    if not isinstance(token, str) or not re.fullmatch(r'[0-9a-f]{32}', token):
        raise ValueError('invalid physical UFS token')
    sources = {}
    for label, directory, pcd, expected in (
        ('bds', 'daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG', 'Probe', b'none\0'),
        ('host', 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG', 'Storage', token.encode() + b'\0')):
        auto = build / directory
        header, source = (auto / 'AutoGen.h').read_text(), (auto / 'AutoGen.c').read_text()
        for flag, number in (('ReadOnlyEnabled', 1), ('ProbeEnabled', 0)):
            if not re.search(r'^#define _PCD_VALUE_PcdDaliUfs' + flag + r'\s+' + str(number) + r'U\s*$', header, re.M):
                raise ValueError('physical UFS ' + label + ' enable/probe PCD mismatch')
        value = re.search(r'_gPcd_FixedAtBuild_PcdDaliUfs' + pcd + r'Token\[(\d+)\]\s*=\s*\{([^}]+)\}', source)
        if (not value or int(value[1]) != len(expected) or
                bytes(int(v.strip(), 0) for v in value[2].split(',') if v.strip()) != expected):
            raise ValueError('physical UFS ' + label + ' token mismatch')
        sources[label] = source
    modules = {}
    for name, guid in PHYSICAL_UFS_MODULES.items():
        module = (build / (name + '.efi')).read_bytes()
        if extract_module(fd, guid, name) != module:
            raise ValueError('final FD physical UFS module differs: ' + name)
        if len(module) < 64:
            raise ValueError('truncated physical UFS PE module: ' + name)
        pe = struct.unpack_from('<I', module, 0x3c)[0]
        if (pe > len(module) - 6 or module[pe:pe + 4] != b'PE\0\0' or
                struct.unpack_from('<H', module, pe + 4)[0] != 0xaa64):
            raise ValueError('physical UFS module is not ARM64 PE: ' + name)
        modules[name] = hashlib.sha256(module).hexdigest()
    host_module = (build / 'DaliUfsHostDxe.efi').read_bytes()
    if token.encode() + b'\0' not in host_module:
        raise ValueError('storage token missing from compiled physical host')
    # Null SHA asserts in DEBUG, so source compilation alone is insufficient.
    # Bind the actual host link inputs, alongside its exact final-FD module.
    libraries = (build / 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/OUTPUT/static_library_files.lst').read_bytes()
    entries = libraries.decode().replace('\\', '/').splitlines()
    crypto = [entry.strip() for entry in entries if '/Library/BaseCryptLib' in entry]
    if (len(crypto) != 1 or not crypto[0].endswith(
            '/OpensslPkg/Library/BaseCryptLib/BaseCryptLib/OUTPUT/BaseCryptLib.lib')):
        raise ValueError('physical UFS host requires the actual non-null SHA-256 library')
    probe = _probe_record((build / 'DaliDiagnosticBds.efi').read_bytes(), sources['bds'], None)
    host = {'enabled': True, 'mode': 'read-only-blockio', 'run_token': token, 'log_version': 1,
            'controller_commands': True, 'mmio_writes': True, 'media_writes': False,
            'write_authorized': False, 'physical_storage_qualified': False,
            'host_sha256': modules['DaliUfsHostDxe'], 'host_verified_in_final_fd': True,
            'host_autogen_sha256': hashlib.sha256(sources['host'].encode()).hexdigest(),
            'sha256_implementation': 'OpensslPkg/Library/BaseCryptLib/BaseCryptLib.inf',
            'host_link_libraries_sha256': hashlib.sha256(libraries).hexdigest(),
            'modules_verified_in_final_fd': modules}
    return probe, host


def sha256_file(path, *, offset=0, length=None):
    digest = hashlib.sha256()
    remaining = None if length is None else length
    with Path(path).open('rb', buffering=0) as stream:
        stream.seek(offset)
        while remaining is None or remaining:
            amount = 8 * 1024 * 1024 if remaining is None else min(8 * 1024 * 1024, remaining)
            data = stream.read(amount)
            if not data:
                break
            digest.update(data)
            if remaining is not None:
                remaining -= len(data)
    if remaining not in (None, 0):
        raise ValueError('short image read while verifying Boot stage')
    return digest.hexdigest()


def validate_boot_stage_descriptor(data):
    if not isinstance(data, bytes) or len(data) != BOOT_STAGE_DESCRIPTOR.size:
        raise ValueError('Boot stage descriptor has the wrong fixed size')
    fields = BOOT_STAGE_DESCRIPTOR.unpack(data)
    (magic, version, descriptor_bytes, root_bytes, boot_offset, boot_bytes, partition_bytes,
     first_lba, last_lba, lun, block_size, root_manifest_sha, boot_sha, descriptor_sha) = fields
    prefix = data[:-32]
    if (magic != b'DALIBOOT' or version != 1 or descriptor_bytes != BOOT_STAGE_DESCRIPTOR.size or
            hashlib.sha256(prefix).digest() != descriptor_sha or root_bytes < 1 << 20 or
            boot_offset < root_bytes or boot_offset % (1 << 20) or boot_bytes == 0 or
            boot_offset % 4096 or boot_bytes % 4096 or block_size != 4096 or lun != 2 or
            first_lba != 7_023_103 or last_lba != 15_403_518 or
            partition_bytes != (last_lba - first_lba + 1) * block_size or
            boot_offset + boot_bytes > partition_bytes):
        raise ValueError('Boot stage descriptor has invalid Dali geometry, bounds or digest')
    return {
        'root_image_bytes': root_bytes,
        'boot_stage_offset_bytes': boot_offset,
        'boot_stage_bytes': boot_bytes,
        'partition_bytes': partition_bytes,
        'first_lba': first_lba,
        'last_lba': last_lba,
        'lun': lun,
        'block_size': block_size,
        'root_manifest_sha256': root_manifest_sha.hex(),
        'boot_stage_sha256': boot_sha.hex(),
        'descriptor_sha256': hashlib.sha256(data).hexdigest(),
    }


def ufs_boot_stage_metadata(build, token, *, fd, descriptor_path):
    probe, host = ufs_read_only_metadata(build, token, fd=fd)
    descriptor = Path(descriptor_path).read_bytes()
    parsed = validate_boot_stage_descriptor(descriptor)
    for label, directory in (
        ('bds', 'daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG'),
        ('host', 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG'),
        ('payload', 'daliPkg/Drivers/PayloadDxe/PayloadDxe/DEBUG'),
    ):
        header = (build / directory / 'AutoGen.h').read_text()
        if not re.search(r'^#define _PCD_VALUE_PcdDaliUfsBootStageEnabled\s+1U\s*$', header, re.M):
            raise ValueError('compiled ' + label + ' Boot-stage PCD is disabled')
    actual = extract_module(fd, BOOT_STAGE_FILE_GUID, 'Boot stage descriptor',
                            section_type=0x19, expected_file_type=0x02)
    if actual != descriptor:
        raise ValueError('final FD Boot stage descriptor differs from the composed partition image')
    host.update(mode='boot-stage', boot_stage_enabled=True,
                descriptor_sha256=parsed['descriptor_sha256'],
                descriptor_verified_in_final_fd=True)
    return probe, host, parsed


def validate_boot_stage_artifacts(payload, descriptor_fields, token, descriptor_path, boot_fat,
                                  boot_fat_report_path, partition_image, partition_report_path):
    if (not isinstance(payload, dict) or payload.get('external_boot_stage') is not True or \
            payload.get('readonly') is not True or payload.get('physical_storage') is not False or \
            payload.get('recovery', {}).get('auto_fastboot') is not False or
            payload.get('xnu_run_token') != token or payload.get('recovery', {}).get('run_token') != token):
        raise ValueError('payload does not require manual recovery for an external read-only Boot stage')
    boot_report = json.loads(Path(boot_fat_report_path).read_text())
    if (boot_report.get('bytes') != descriptor_fields['boot_stage_bytes'] or
            boot_report.get('sha256') != descriptor_fields['boot_stage_sha256'] or
            payload.get('bytes') != boot_report.get('bytes') or payload.get('sha256') != boot_report.get('sha256')):
        raise ValueError('external Boot FAT hash/size differs from the firmware descriptor')
    files = boot_report.get('files', {})
    for name in ('EFI/UniXNU/unixnu.toml', 'EFI/UniXNU/dali-boot.conf',
                 'EFI/UniXNU/Payloads/kernel.macho', 'EFI/UniXNU/Payloads/kernel.macho.adt',
                 'EFI/UniXNU/Payloads/root.img', 'EFI/UniXNU/Payloads/kernel.macho.trustcache'):
        if name not in files:
            raise ValueError('external Boot FAT lacks required iPadOS boot member: ' + name)
    if (files['EFI/UniXNU/Payloads/kernel.macho']['sha256'] != payload.get('xnu_kernel_sha256') or
            files['EFI/UniXNU/Payloads/kernel.macho.adt']['sha256'] != payload.get('xnu_adt_sha256') or
            files['EFI/UniXNU/Payloads/root.img']['sha256'] != payload.get('xnu_initrd_sha256') or
            files['EFI/UniXNU/Payloads/kernel.macho.trustcache']['sha256'] != payload.get('xnu_trust_cache_sha256')):
        raise ValueError('external Boot FAT members differ from payload input hashes')
    if sha256_file(boot_fat) != descriptor_fields['boot_stage_sha256']:
        raise ValueError('external Boot FAT file changed after firmware descriptor generation')

    record = json.loads(Path(partition_report_path).read_text())
    if (record.get('root_windows') != ['OS', 'SystemOS'] or
            record.get('root_image_bytes') != descriptor_fields['root_image_bytes'] or
            record.get('root_manifest_sha256') != descriptor_fields['root_manifest_sha256'] or
            record.get('boot_stage_offset_bytes') != descriptor_fields['boot_stage_offset_bytes'] or
            record.get('boot_stage_bytes') != descriptor_fields['boot_stage_bytes'] or
            record.get('boot_stage_sha256') != descriptor_fields['boot_stage_sha256'] or
            record.get('partition_bytes') != descriptor_fields['partition_bytes'] or
            record.get('lun') != 2 or record.get('physical_ufs_qualified') is not False):
        raise ValueError('composite unixnu_data report contradicts the firmware descriptor')
    descriptor = Path(descriptor_path).read_bytes()
    if hashlib.sha256(descriptor).hexdigest() != record.get('firmware_descriptor_sha256'):
        raise ValueError('partition report does not bind the embedded descriptor')
    image = Path(partition_image)
    if image.stat().st_size != record.get('partition_image_bytes') or \
            sha256_file(image) != record.get('partition_image_sha256'):
        raise ValueError('composite unixnu_data image does not match its report')
    if sha256_file(image, offset=descriptor_fields['boot_stage_offset_bytes'],
                   length=descriptor_fields['boot_stage_bytes']) != descriptor_fields['boot_stage_sha256']:
        raise ValueError('composite unixnu_data Boot region differs from the pinned Boot FAT')
    return {
        'enabled': True, 'mode': 'read-only-ufs-boot-stage', 'lun': 2,
        'descriptor_sha256': descriptor_fields['descriptor_sha256'],
        'partition_image_bytes': record['partition_image_bytes'],
        'partition_image_sha256': record['partition_image_sha256'],
        'boot_stage_offset_bytes': descriptor_fields['boot_stage_offset_bytes'],
        'boot_stage_bytes': descriptor_fields['boot_stage_bytes'],
        'boot_stage_sha256': descriptor_fields['boot_stage_sha256'],
        'physical_ufs_qualified': False, 'media_writes_qualified': False,
    }


def validate_diagnostic_payload(payload):
    if (not isinstance(payload, dict) or not isinstance(payload.get('recovery'), dict) or
            payload.get('xnu_kernel_sha256') is not None or payload.get('xnu_ramdisk_sha256') is not None or
            payload['recovery'].get('auto_fastboot') is not False or
            payload['recovery'].get('reset_method') is not None):
        raise ValueError('UFS diagnostic requires a UEFI-only payload with automatic recovery disabled')


def validate_xnu_ufs_payload(payload, token):
    if (not isinstance(payload, dict) or not isinstance(payload.get('recovery'), dict) or
            payload.get('xnu_kernel_sha256') is None or payload.get('xnu_adt_sha256') is None or
            payload.get('xnu_ramdisk_sha256') is not None or payload.get('xnu_trust_cache_sha256') is not None or
            payload.get('xnu_run_token') != token or
            payload['recovery'].get('auto_fastboot') is not False or
            payload['recovery'].get('reset_method') is not None):
        raise ValueError('XNU UFS diagnostic needs a source kernel, no PID1/reset and manual recovery')
    profile = payload.get('xnu_ufs_read_only')
    if (not isinstance(profile, dict) or profile.get('enabled') is not True or
            profile.get('run_token') != token or profile.get('write_authorized') is not False or
            profile.get('physical_ufs_qualified') is not False or
            not re.fullmatch(r'[0-9a-f]{64}', profile.get('live_fdt_sha256', '')) or
            profile.get('resources_fdt_sha256') != profile.get('live_fdt_sha256')):
        raise ValueError('XNU UFS read-only profile/token/FDT binding mismatch')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--backup", type=Path, required=True)
    p.add_argument("--mode", choices=["DEBUG"], default="DEBUG")
    p.add_argument("--mu", type=Path, required=True)
    p.add_argument("--unixnu", type=Path, required=True, help="reviewed generic UniXNU checkout")
    p.add_argument("--device-repo", type=Path, required=True, help="pinned UniXNU-Dali checkout")
    p.add_argument("--out", type=Path, required=True)
    profile = p.add_mutually_exclusive_group()
    profile.add_argument("--ufs-probe-token", help="bind opt-in read-only firmware register snapshot")
    profile.add_argument("--ufs-read-only-token", help="bind the explicit physical read-only BlockIo diagnostic")
    profile.add_argument("--ufs-boot-stage-token", help="bind external read-only Boot FAT to the final firmware/data images")
    profile.add_argument("--ufs-xnu-read-only-token", help="bind the combined firmware/XNU read-only ownership trial")
    p.add_argument("--ufs-xnu-live-fdt", type=Path, help="live raw FDT used to validate the XNU UFS ADT node")
    p.add_argument("--boot-stage-descriptor", type=Path)
    p.add_argument("--boot-fat", type=Path)
    p.add_argument("--boot-fat-report", type=Path)
    p.add_argument("--userdata-image", type=Path)
    p.add_argument("--userdata-report", type=Path)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    mu = args.mu.resolve()
    unixnu = args.unixnu.resolve()
    device_repo = args.device_repo.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.backup / "manifest.json").read_text())
    board = tomllib.loads((device_repo / "board.toml").read_text())
    firmware = board.get("firmware", {})
    uefi_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    unixnu_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=unixnu, text=True).strip()
    if firmware.get("uefi_commit") != uefi_commit or firmware.get("unixnu_commit") != unixnu_commit:
        raise SystemExit("UEFI/UniXNU checkout differs from the device board.toml commit pins")
    if not manifest.get("complete") or manifest.get("product") != "dali":
        raise SystemExit("A complete verified dali backup is required")
    expected_ramoops = {"mem_address": 0xAEC00000, "mem_size": 0x410000, "record_size": 4096,
                        "console_size": 0x200000, "pmsg_size": 0x200000, "ftrace_size": 4096, "ecc": 0, "mem_type": 0}
    if manifest.get("ramoops") != expected_ramoops:
        raise SystemExit("Ramoops parameters differ or were not backed up: review the console layout before packaging")
    for name in ("boot_b.img", "runtime.dtb"):
        data = (args.backup / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != manifest["files"][name]["sha256"]:
            raise SystemExit("Backup checksum mismatch: " + name)
    tree = Fdt((args.backup / "runtime.dtb").read_bytes())
    layout = tree.validate()
    io_layout = tree.validate_io()
    boot = (args.backup / "boot_b.img").read_bytes()
    if boot[:8] != b"ANDROID!" or struct.unpack_from("<I", boot, 40)[0] != 4:
        raise SystemExit("Expected boot header v4")
    if boot[4096:4100] != bytes.fromhex("02214c18"):
        raise SystemExit("Expected the observed legacy LZ4 kernel compression")
    config = tomllib.loads((root / "Resources/Configs/dali.toml").read_text())
    fd_base = config["uefi_fd"]["base"]
    fd_size = config["uefi_fd"]["size"]
    if (fd_base, fd_size) != (0xC1000000, 0x4000000):
        raise SystemExit("Dali FD address/size differs from the reviewed 64 MiB arena layout")
    fd = mu / f"Build/daliPkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd"
    if fd.stat().st_size != fd_size:
        raise SystemExit("Unexpected dali FD size")
    loader_measurement = None
    boot_stage_fields = None
    try:
        if args.ufs_boot_stage_token is not None:
            required = (args.boot_stage_descriptor, args.boot_fat, args.boot_fat_report,
                        args.userdata_image, args.userdata_report)
            if any(path is None or not path.is_file() for path in required):
                raise ValueError('Boot-stage package requires descriptor, Boot FAT/report and composite userdata image/report')
            probe, host, boot_stage_fields = ufs_boot_stage_metadata(
                fd.parent.parent / 'AARCH64', args.ufs_boot_stage_token,
                fd=fd.read_bytes(), descriptor_path=args.boot_stage_descriptor)
        elif args.ufs_xnu_read_only_token is not None:
            probe, host = ufs_read_only_metadata(fd.parent.parent / "AARCH64", args.ufs_xnu_read_only_token, fd=fd.read_bytes())
            loader_measurement = loader_measurement_metadata(fd.parent.parent / "AARCH64", unixnu, fd=fd.read_bytes())
        elif args.ufs_read_only_token is not None:
            probe, host = ufs_read_only_metadata(fd.parent.parent / "AARCH64", args.ufs_read_only_token, fd=fd.read_bytes())
        else:
            probe = ufs_probe_metadata(fd.parent.parent / "AARCH64", args.ufs_probe_token, fd=fd.read_bytes())
            host = ufs_host_off_metadata(fd.parent.parent / "AARCH64", fd=fd.read_bytes())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit("UFS probe build verification failed: " + str(error)) from None
    payload = json.loads((out / "payload.json").read_text())
    boot_stage_record = None
    if args.ufs_boot_stage_token is not None:
        try:
            boot_stage_record = validate_boot_stage_artifacts(
                payload, boot_stage_fields, args.ufs_boot_stage_token, args.boot_stage_descriptor, args.boot_fat,
                args.boot_fat_report, args.userdata_image, args.userdata_report)
            boot_stage_record['run_token'] = args.ufs_boot_stage_token
        except (OSError, ValueError, UnicodeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
            raise SystemExit('Boot-stage image verification failed: ' + str(error)) from None
    elif args.ufs_xnu_read_only_token is not None:
        try:
            validate_xnu_ufs_payload(payload, args.ufs_xnu_read_only_token)
            if args.ufs_xnu_live_fdt is None:
                raise ValueError('XNU UFS package requires the candidate live FDT')
            live_fdt = args.ufs_xnu_live_fdt.read_bytes()
            if hashlib.sha256(live_fdt).hexdigest() != payload['xnu_ufs_read_only']['live_fdt_sha256']:
                raise ValueError('candidate live FDT differs from the payload binding')
            raw_payload = extract_module(fd.read_bytes(), XNU_UFS_PAYLOAD_GUID,
                                         'XNU UFS payload', section_type=0x19, expected_file_type=0x02)
            payload_image = (out / 'payload.img').read_bytes()
            if (raw_payload != payload_image or len(raw_payload) != payload.get('bytes') or
                    hashlib.sha256(raw_payload).hexdigest() != payload.get('sha256')):
                raise ValueError('actual final FD RAW payload differs from the staged payload image')
            config_bytes = subprocess.check_output(['mtype', '-i', str(out / 'payload.img'), '::/EFI/UniXNU/unixnu.toml'])
            recovery_bytes = subprocess.check_output(['mtype', '-i', str(out / 'payload.img'), '::/EFI/UniXNU/dali-boot.conf'])
            adt_bytes = subprocess.check_output(['mtype', '-i', str(out / 'payload.img'), '::/EFI/UniXNU/Payloads/kernel.macho.adt'])
            kernel_bytes = subprocess.check_output(['mtype', '-i', str(out / 'payload.img'), '::/EFI/UniXNU/Payloads/kernel.macho'])
            sys.path.insert(0, str(root / 'tools'))
            from make_payload import validate_xnu_ufs_bundle
            validate_xnu_ufs_bundle(payload, args.ufs_xnu_read_only_token, adt_bytes, live_fdt,
                                    config_bytes, recovery_bytes, kernel_bytes)
        except (OSError, ValueError, UnicodeError, subprocess.SubprocessError) as error:
            raise SystemExit('XNU UFS payload verification failed: ' + str(error)) from None
    elif args.ufs_xnu_live_fdt is not None:
        raise SystemExit('--ufs-xnu-live-fdt requires --ufs-xnu-read-only-token')
    if (probe['enabled'] or host['enabled']) and args.ufs_boot_stage_token is None:
        try:
            if args.ufs_xnu_read_only_token is None:
                validate_diagnostic_payload(payload)
        except ValueError as error:
            raise SystemExit(str(error)) from None
    shim_obj, shim_elf, shim = out / "DaliShim.o", out / "DaliShim.elf", out / "DaliShim.bin"
    subprocess.run(["clang", "--target=aarch64-none-elf", "-c", str(mu / "BootShim/DaliShim.S"),
                    "-o", str(shim_obj), f"-DFD_BASE={fd_base:#x}", f"-DFD_SIZE={fd_size:#x}"], check=True)
    subprocess.run(["ld.lld", "-m", "aarch64elf", "--image-base=0", "-Ttext=0", "-e", "_start",
                    str(shim_obj), "-o", str(shim_elf)], check=True)
    subprocess.run(["llvm-objcopy", "-O", "binary", str(shim_elf), str(shim)], check=True)
    if shim.stat().st_size != 4096:
        raise SystemExit("Unexpected shim size or unresolved literal pool")
    raw = shim.read_bytes() + fd.read_bytes()
    if raw[56:60] != b"ARM\x64" or struct.unpack_from("<Q", raw, 16)[0] != len(raw):
        raise SystemExit("Invalid ARM64 Image header")
    (out / "Image").write_bytes(raw)
    compressed = subprocess.check_output(["lz4", "-l", "-c"], input=raw)
    if subprocess.check_output(["lz4", "-d", "-c"], input=compressed) != raw:
        raise SystemExit("LZ4 round trip failed")
    # Keep the original boot-v4 metadata and command line; only replace the kernel.
    header = bytearray(boot[:4096])
    struct.pack_into("<I", header, 8, len(compressed))
    struct.pack_into("<I", header, 12, 0)  # no generic ramdisk; init_boot remains separate
    struct.pack_into("<I", header, 1580, 0)  # old GKI boot signature does not cover this image
    image = bytes(header) + compressed
    image += bytes((-len(image)) % 4096)
    limit = manifest["files"]["boot_b.img"]["bytes"]
    if len(image) > limit:
        raise SystemExit("Boot image exceeds test boot partition")
    target = out / "UniXNU-dali-uefi-boot.img"
    target.write_bytes(image)
    record = {"experimental": True, "target": "dali", "mode": args.mode,
              "image": str(target), "bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
              "fd_base": fd_base, "fd_size": fd_size,
              "fd_sha256": hashlib.sha256(fd.read_bytes()).hexdigest(), "layout": layout, "io_layout": io_layout,
              "upstream_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=mu, text=True).strip(),
              "backup_manifest": str((args.backup / "manifest.json").resolve()),
              "unixnu_commit": unixnu_commit,
              "device_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=device_repo, text=True).strip(),
              "uefi_commit": uefi_commit,
              "mu_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=mu, text=True).strip(),
              "payload": payload, "ufs_probe": probe, "ufs_host": host,
              "boot_stage": boot_stage_record,
              "loader_measurement": loader_measurement,
              "ufs_xnu": ({"enabled": True, "run_token": args.ufs_xnu_read_only_token,
                           "live_fdt_sha256": payload['xnu_ufs_read_only']['live_fdt_sha256'],
                           "write_authorized": False, "physical_ufs_qualified": False}
                          if args.ufs_xnu_read_only_token is not None else
                          {"enabled": False, "write_authorized": False, "physical_ufs_qualified": False})}
    (out / "manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
