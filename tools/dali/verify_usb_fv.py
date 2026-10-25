#!/usr/bin/env python3
"""Verify source USB/class PE modules inside the actual compressed Dali FD."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import uuid

from firmware_volume import extract_module

MODULES = {
    'XhciDxe': 'MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf',
    **{name: f'MdeModulePkg/Bus/Usb/{name}/{name}.inf' for name in (
        'UsbBusDxe', 'UsbKbDxe', 'UsbMouseDxe', 'UsbMassStorageDxe')},
    **{name: f'MdeModulePkg/Universal/Disk/{name}/{name}.inf' for name in ('DiskIoDxe', 'PartitionDxe')},
    **{name: f'MdeModulePkg/Universal/Console/{name}/{name}.inf' for name in ('ConPlatformDxe', 'ConSplitterDxe')},
    'Fat': 'FatPkg/EnhancedFatDxe/Fat.inf',
}


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True, help='DEBUG_CLANGPDB directory with AARCH64 images')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resource-context', action='store_true', help='also verify the opt-in native CPU mapping/FDT context')
    parser.add_argument('--native-host-worker', action='store_true', help='also verify the opt-in fail-closed native host consumer')
    parser.add_argument('--default-off', action='store_true', help='verify that the optional USB stack/context/worker are absent')
    args = parser.parse_args()
    if args.default_off and (args.resource_context or args.native_host_worker):
        parser.error('--default-off cannot be combined with enabled module options')
    if args.native_host_worker and not args.resource_context:
        parser.error('--native-host-worker requires --resource-context')
    fd = args.fd.read_bytes(); modules = {}
    if args.default_off:
        inputs = [(name, MODULES[name], root / 'Mu_Basecore' / MODULES[name]) for name in ('DiskIoDxe', 'Fat')]
    else:
        inputs = [(name, inf, root / 'Mu_Basecore' / inf) for name, inf in MODULES.items()]
    if args.resource_context:
        inf = 'Platforms/Xiaomi/daliPkg/Drivers/UsbResourceContextDxe/UsbResourceContextDxe.inf'
        inputs.append(('DaliUsbResourceContextDxe', inf, root / inf))
    if args.native_host_worker:
        inf = 'Platforms/Xiaomi/daliPkg/Drivers/UsbNativeHostWorkerDxe/UsbNativeHostWorkerDxe.inf'
        inputs.append(('DaliUsbNativeHostWorkerDxe', inf, root / inf))
    for name, inf, source in inputs:
        matches = re.findall(r'^\s*FILE_GUID\s*=\s*([0-9a-fA-F-]{36})\s*$', source.read_text(), re.M)
        if len(matches) != 1:
            raise ValueError(f'ambiguous source FILE_GUID: {inf}')
        image = extract_module(fd, uuid.UUID(matches[0]).bytes_le)
        expected = args.build / 'AARCH64' / (name + '.efi')
        if image != expected.read_bytes():
            raise ValueError(f'final FD differs from compiled source module: {name}')
        modules[name] = {'file_guid': matches[0], 'bytes': len(image),
                         'sha256': hashlib.sha256(image).hexdigest(), 'matches_final_fd': True,
                         'compiled_image': str(expected.resolve()), 'source_inf': inf,
                         'source_inf_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
    absent = []
    if args.default_off:
        optional = dict(MODULES)
        optional.pop('DiskIoDxe')
        optional.pop('Fat')
        optional['DaliUsbResourceContextDxe'] = 'Platforms/Xiaomi/daliPkg/Drivers/UsbResourceContextDxe/UsbResourceContextDxe.inf'
        optional['DaliUsbNativeHostWorkerDxe'] = 'Platforms/Xiaomi/daliPkg/Drivers/UsbNativeHostWorkerDxe/UsbNativeHostWorkerDxe.inf'
        for name, inf in optional.items():
            source = root / 'Mu_Basecore' / inf if inf.startswith(('MdeModulePkg/', 'FatPkg/')) else root / inf
            matches = re.findall(r'^\s*FILE_GUID\s*=\s*([0-9a-fA-F-]{36})\s*$', source.read_text(), re.M)
            if len(matches) != 1:
                raise ValueError(f'ambiguous source FILE_GUID: {inf}')
            try:
                extract_module(fd, uuid.UUID(matches[0]).bytes_le)
            except ValueError as error:
                if str(error) != 'final FD must contain exactly one selected PE32 module':
                    raise
            else:
                raise ValueError(f'default-off final FD unexpectedly contains {name}')
            absent.append(name)
    result = {'scope': 'Dali opt-in source USB/class FV assembly; no native controller/DMA/role or hardware qualification',
              'hardware_qualified': False, 'final_fd': str(args.fd.resolve()),
              'resource_context_requested': args.resource_context,
              'native_host_worker_requested': args.native_host_worker,
              'default_off_asserted': args.default_off,
              'final_fd_sha256': hashlib.sha256(fd).hexdigest(), 'modules': modules,
              'absent_optional_modules': absent,
              'verifier_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'parser_sha256': hashlib.sha256((Path(__file__).parent / 'firmware_volume.py').read_bytes()).hexdigest()}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
