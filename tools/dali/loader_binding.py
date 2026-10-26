"""Bind the final-FD loader to pinned local build inputs; no write grant issuer.

This is build-product consistency, not a signature or an independent compiler
attestation. The normal build/audit must establish the actual code behavior.
"""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import uuid

from firmware_volume import extract_module

DARWINBOOT_GUID = uuid.UUID('1b64af87-8415-4b57-9b16-680d70913b9f').bytes_le
OBJECTS = ('DarwinBoot.obj', 'StorageHandoff.obj', 'KernelMeasurement.obj', 'Entry.obj', 'AutoGen.obj')
SOURCE_FILES = ('DarwinBoot.c', 'DarwinBoot.inf', 'KernelMeasurement.c', 'KernelMeasurement.h',
                'StorageHandoff.c', 'StorageHandoff.h', 'BootArgs.h', 'BootVideo.h',
                'PackedRamdisk.h', 'WindowSize.h', 'DiagFont.h', 'Entry.S')


def clean_revision(path):
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=path, text=True).strip()
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('invalid loader source revision')
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=all'], cwd=path):
        raise ValueError('loader source checkout must be clean')
    return revision


def loader_measurement_metadata(build, unixnu, *, fd):
    build, unixnu = Path(build).resolve(), Path(unixnu).resolve()
    loader = unixnu / 'drivers/DarwinBoot'
    generic_revision, loader_revision = clean_revision(unixnu), clean_revision(loader)
    gitlink = subprocess.check_output(['git', 'ls-tree', 'HEAD', '--', 'drivers/DarwinBoot'],
                                     cwd=unixnu, text=True).strip()
    if gitlink != '160000 commit ' + loader_revision + '\tdrivers/DarwinBoot':
        raise ValueError('DarwinBoot checkout differs from the pinned generic gitlink')
    module = (build / 'DarwinBoot.efi').read_bytes()
    if extract_module(fd, DARWINBOOT_GUID, 'DarwinBoot', expected_file_type=7) != module:
        raise ValueError('final FD DarwinBoot differs from the compiled loader')
    if len(module) < 64:
        raise ValueError('truncated DarwinBoot PE')
    pe = struct.unpack_from('<I', module, 0x3c)[0]
    if (pe > len(module)-6 or module[pe:pe+4] != b'PE\0\0' or
            struct.unpack_from('<H', module, pe+4)[0] != 0xaa64):
        raise ValueError('DarwinBoot is not ARM64 PE')
    if b'darwinboot-kernel-sha256\0' not in module:
        raise ValueError('DarwinBoot lacks the kernel measurement property contract')
    output = build / 'DarwinBootPkg/Driver/DarwinBoot/OUTPUT'
    object_list = (output / 'object_files.lst').read_bytes()
    paths = [Path(line.strip()).resolve() for line in object_list.decode().splitlines() if line.strip()]
    expected = [output / name for name in OBJECTS]
    if paths != expected:
        raise ValueError('DarwinBoot measurement object/link inputs differ')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    objects = {}
    for path in paths:
        data = path.read_bytes()
        if not data:
            raise ValueError('empty DarwinBoot build object')
        objects[path.name] = hashlib.sha256(data).hexdigest()
    sources = {name:digest(loader / 'DarwinBootPkg/Driver' / name) for name in SOURCE_FILES}
    return {'enabled':True, 'contract':'darwinboot-kernel-sha256-v1',
            'unixnu_commit':generic_revision, 'darwinboot_commit':loader_revision,
            'loader_sha256':hashlib.sha256(module).hexdigest(), 'loader_verified_in_final_fd':True,
            'source_sha256':sources, 'object_sha256':objects,
            'object_list_sha256':hashlib.sha256(object_list).hexdigest(),
            'link_libraries_sha256':digest(output / 'static_library_files.lst'),
            'runtime_provenance_authenticated':False, 'physical_write_authorized':False}
