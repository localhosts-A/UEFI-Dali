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
    args = parser.parse_args()
    fd = args.fd.read_bytes(); modules = {}
    for name, inf in MODULES.items():
        source = root / 'Mu_Basecore' / inf
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
    result = {'scope': 'Dali opt-in source USB/class FV assembly; no native controller/DMA/role or hardware qualification',
              'hardware_qualified': False, 'final_fd': str(args.fd.resolve()),
              'final_fd_sha256': hashlib.sha256(fd).hexdigest(), 'modules': modules,
              'verifier_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'parser_sha256': hashlib.sha256((Path(__file__).parent / 'firmware_volume.py').read_bytes()).hexdigest()}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
