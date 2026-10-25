#!/usr/bin/env python3
"""Compile the pinned source USB/HID/BOT/partition/FAT drivers and QEMU-only app."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

NAMES = ('XhciDxe', 'UsbBusDxe', 'UsbKbDxe', 'UsbMouseDxe', 'UsbMassStorageDxe',
         'DiskIoDxe', 'PartitionDxe', 'Fat', 'UsbQemuTest')


def source_tree(path):
    """Identify tracked edits, untracked inputs and each initialized submodule."""
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=path)
    untracked = git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0')
    result = {'commit': git('rev-parse', 'HEAD').decode().strip(),
              'diff_sha256': hashlib.sha256(git('diff', '--binary', 'HEAD')).hexdigest(),
              'untracked_sha256': {os.fsdecode(p): hashlib.sha256((path / os.fsdecode(p)).read_bytes()).hexdigest()
                                   for p in untracked if p}, 'submodules': {}}
    for entry in git('ls-files', '--stage', '-z').split(b'\0'):
        if entry.startswith(b'160000 '):
            name = os.fsdecode(entry.split(b'\t', 1)[1]); child = path / name
            result['submodules'][name] = source_tree(child) if (child / '.git').exists() else {'initialized': False}
    return result


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    edk = root / 'Mu_Basecore'
    generic = root.parent / 'UniXNU'
    if output.is_relative_to(root) or root.is_relative_to(output) or output.is_relative_to(generic) or generic.is_relative_to(output):
        parser.error('output must be separate from firmware source/dependencies')
    tools = edk / 'BaseTools'
    clang = shutil.which('clang')
    if not clang or not (tools / 'Source/C/bin/GenFw').is_file():
        parser.error('clang and initialized isolated BaseTools are required')
    conf = output / 'Conf'
    conf.mkdir(parents=True, exist_ok=True)
    (output / 'usb-qemu-build.json').unlink(missing_ok=True)
    for name in ('target', 'tools_def', 'build_rule'):
        shutil.copyfile(tools / 'Conf' / (name + '.template'), conf / (name + '.txt'))
    env = os.environ.copy()
    env.update(WORKSPACE=str(output), CONF_PATH=str(conf), EDK_TOOLS_PATH=str(tools),
               PACKAGES_PATH=os.pathsep.join(map(str, (root / 'Platforms/Xiaomi', root / 'Silicon/Silicium', generic / 'package', edk))),
               CLANG_BIN=str(Path(clang).parent) + '/', PYTHON_COMMAND=sys.executable)
    env['PATH'] = os.pathsep.join((str(tools / 'BinWrappers/PosixLike'), str(tools / 'Source/C/bin'), env.get('PATH', '')))
    command = [str(tools / 'BinWrappers/PosixLike/build'), '-p', 'daliPkg/UsbQemu.dsc',
               '-a', 'AARCH64', '-b', 'DEBUG', '-t', 'CLANGPDB', '-n', '4']
    subprocess.run(command, cwd=output, env=env, check=True)
    usb_bus_dir = output / 'Build/DaliUsbQemu/DEBUG_CLANGPDB/AARCH64/MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe'
    usb_bus_archive = usb_bus_dir / 'OUTPUT/UsbBusDxe.lib'
    ar = shutil.which('llvm-ar')
    nm = shutil.which('llvm-nm')
    if not ar or not nm or not usb_bus_archive.is_file():
        raise RuntimeError('llvm-ar, llvm-nm and the actual UsbBusDxe archive are required for the multi-TT source gate')
    members = subprocess.check_output([ar, 't', str(usb_bus_archive)], text=True).splitlines()
    symbol_output = subprocess.check_output([nm, '--defined-only', str(usb_bus_archive)], text=True)
    undefined_output = subprocess.check_output([nm, '--undefined-only', str(usb_bus_archive)], text=True)
    if not any(member.endswith('/UsbHubMultiTt.obj') or member == 'UsbHubMultiTt.obj' for member in members):
        raise RuntimeError('actual UsbBusDxe archive does not contain UsbHubMultiTt.obj')
    if not any(line.split()[-1:] == ['UsbHubSelectMultiTtSetting'] for line in symbol_output.splitlines()):
        raise RuntimeError('actual UsbBusDxe archive does not define UsbHubSelectMultiTtSetting')
    if 'UsbHub.obj:' not in undefined_output or 'U UsbHubSelectMultiTtSetting' not in undefined_output:
        raise RuntimeError('actual UsbHub object does not reference the multi-TT selector')
    artifacts = {}
    for name in NAMES:
        image = output / 'Build/DaliUsbQemu/DEBUG_CLANGPDB/AARCH64' / (name + '.efi')
        data = image.read_bytes()
        pe = int.from_bytes(data[60:64], 'little'); optional = pe + 24
        if len(data) < 64 or data[:2] != b'MZ' or pe < 64 or optional + 112 > len(data) or \
                data[pe:pe + 4] != b'PE\0\0' or int.from_bytes(data[pe + 4:pe + 6], 'little') != 0xaa64 or \
                int.from_bytes(data[pe + 20:pe + 22], 'little') < 112 or \
                int.from_bytes(data[optional:optional + 2], 'little') != 0x20b or \
                int.from_bytes(data[optional + 68:optional + 70], 'little') != (10 if name == 'UsbQemuTest' else 11):
            raise RuntimeError(f'not an actual AArch64 PE image: {image}')
        artifacts[name] = {'path': str(image), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    result = {'scope': 'source drivers/application; external QEMU platform firmware is separate, no Dali native controller proof',
              'command': command, 'artifacts': artifacts, 'hardware_qualified': False,
              'source_inputs': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in (
                  'Platforms/Xiaomi/daliPkg/UsbQemu.dsc',
                  'Platforms/Xiaomi/daliPkg/Applications/UsbQemuTest/UsbQemuTest.c',
                  'Platforms/Xiaomi/daliPkg/Applications/UsbQemuTest/UsbQemuTest.inf',
                  'tools/dali/build_usb_qemu.py',
                  'tools/dali/test_usb_hub_multitt.py',
                  'tools/dali/fixtures/usb_hub_multitt_test.c',
                  'tools/dali/README-usb-native-host.md',
                  'Resources/MuPatches/UsbHub-MultiTT.patch')},
              'usb_hub_multi_tt': {'archive': str(usb_bus_archive),
                                   'archive_sha256': hashlib.sha256(usb_bus_archive.read_bytes()).hexdigest(),
                                   'member': next(member for member in members if member.endswith('/UsbHubMultiTt.obj') or member == 'UsbHubMultiTt.obj'),
                                   'defined_symbol': 'UsbHubSelectMultiTtSetting',
                                   'referenced_by': 'UsbHub.obj'},
              'mu_reviewed_diff_sha256': hashlib.sha256(subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=edk)).hexdigest(),
              'mu_basecore_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=edk, text=True).strip(),
              'source_tree': source_tree(root), 'python': sys.version,
              'generic_source_tree': source_tree(generic),
              'clang': subprocess.check_output([clang, '--version'], text=True).splitlines()[0]}
    (output / 'usb-qemu-build.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
