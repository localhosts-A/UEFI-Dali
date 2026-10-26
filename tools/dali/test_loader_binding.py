"""Final-FD loader/pin consistency; synthetic PE fixtures do not test SHA code."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from firmware_volume import extract_module
from loader_binding import DARWINBOOT_GUID, OBJECTS, SOURCE_FILES, loader_measurement_metadata
from test_ufs_package import firmware


class LoaderBindingTests(unittest.TestCase):
    def git(self, where, *args):
        env = dict(os.environ, GIT_AUTHOR_NAME='fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                   GIT_COMMITTER_NAME='fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')
        return subprocess.check_output(['git', *args], cwd=where, env=env, stderr=subprocess.PIPE, text=True).strip()

    def fixture(self, root):
        generic = root / 'generic'; loader = generic / 'drivers/DarwinBoot'
        loader.mkdir(parents=True)
        self.git(generic,'init','-q'); self.git(loader,'init','-q')
        source = loader / 'DarwinBootPkg/Driver'; source.mkdir(parents=True)
        for name in SOURCE_FILES:
            (source / name).write_text('fixture-' + name)
        self.git(loader,'add','.'); self.git(loader,'commit','-qm','fixture')
        revision = self.git(loader,'rev-parse','HEAD')
        self.git(generic,'update-index','--add','--cacheinfo','160000,' + revision + ',drivers/DarwinBoot')
        self.git(generic,'commit','-qm','pin fixture')
        build = root / 'build'; output = build / 'DarwinBootPkg/Driver/DarwinBoot/OUTPUT'
        output.mkdir(parents=True)
        paths=[]
        for name in OBJECTS:
            path=output/name; path.write_bytes(b'fixture-object-' + name.encode()); paths.append(str(path))
        (output/'object_files.lst').write_text('\n'.join(paths)+'\n')
        (output/'static_library_files.lst').write_text('/fixture/BaseMemoryLib.lib\n')
        data=bytearray(128); data[:2]=b'MZ'; struct.pack_into('<I',data,0x3c,64)
        data[64:68]=b'PE\0\0'; struct.pack_into('<H',data,68,0xaa64)
        data+=b'darwinboot-kernel-sha256\0'
        (build/'DarwinBoot.efi').write_bytes(data)
        return generic,loader,build,output,firmware(bytes(data),nested=True,guid=DARWINBOOT_GUID)

    def test_final_fd_bytes_pins_sources_and_objects_are_bound_without_authorizing_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            generic,loader,build,output,fd=self.fixture(Path(temp))
            result=loader_measurement_metadata(build,generic,fd=fd)
            self.assertEqual(result['darwinboot_commit'],self.git(loader,'rev-parse','HEAD'))
            self.assertEqual(result['unixnu_commit'],self.git(generic,'rev-parse','HEAD'))
            self.assertEqual(result['loader_sha256'],hashlib.sha256((build/'DarwinBoot.efi').read_bytes()).hexdigest())
            self.assertEqual(set(result['source_sha256']),set(SOURCE_FILES))
            self.assertEqual(set(result['object_sha256']),set(OBJECTS))
            self.assertTrue(result['loader_verified_in_final_fd'])
            self.assertFalse(result['runtime_provenance_authenticated'])
            self.assertFalse(result['physical_write_authorized'])
            (output/'KernelMeasurement.obj').write_bytes(b'changed actual object')
            changed=loader_measurement_metadata(build,generic,fd=fd)
            self.assertNotEqual(changed['object_sha256'],result['object_sha256'])

    def test_missing_duplicate_and_stale_loader_refuse(self):
        with tempfile.TemporaryDirectory() as temp:
            generic,_,build,_,fd=self.fixture(Path(temp))
            module=extract_module(fd,DARWINBOOT_GUID)
            for changed in (firmware(module,guid=bytes(16)),firmware(module,duplicate=True,guid=DARWINBOOT_GUID),
                            firmware(module+b'changed',guid=DARWINBOOT_GUID)):
                with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=changed)
            (build/'DarwinBoot.efi').write_bytes(module+b'changed')
            with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=fd)

    def test_wrong_machine_and_old_loader_without_measurement_contract_refuse(self):
        with tempfile.TemporaryDirectory() as temp:
            generic,_,build,_,fd=self.fixture(Path(temp))
            original=extract_module(fd,DARWINBOOT_GUID)
            wrong=bytearray(original); wrong[68:70]=b'\x64\x86'
            for module in (bytes(wrong),original[:128]):
                (build/'DarwinBoot.efi').write_bytes(module)
                with self.assertRaises(ValueError):
                    loader_measurement_metadata(build,generic,fd=firmware(module,guid=DARWINBOOT_GUID))

    def test_dirty_sources_and_unpinned_submodule_refuse(self):
        with tempfile.TemporaryDirectory() as temp:
            generic,loader,build,_,fd=self.fixture(Path(temp))
            source=loader/'DarwinBootPkg/Driver/KernelMeasurement.c'
            original=source.read_bytes(); source.write_bytes(b'changed source')
            with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=fd)
            source.write_bytes(original)
            extra=generic/'extra.c'; extra.write_text('untracked source')
            with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=fd)
            extra.unlink()
            (loader/'extra.c').write_text('extra loader source')
            self.git(loader,'add','.'); self.git(loader,'commit','-qm','unpinned loader')
            with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=fd)

    def test_missing_duplicate_extra_and_escaped_objects_refuse(self):
        with tempfile.TemporaryDirectory() as temp:
            generic,_,build,output,fd=self.fixture(Path(temp))
            path=output/'object_files.lst'; original=path.read_text(); lines=original.splitlines()
            for value in ('\n'.join(lines[:-1])+'\n',original+lines[2]+'\n',
                          original+'/tmp/other.obj\n',original.replace(lines[2],'/tmp/KernelMeasurement.obj')):
                path.write_text(value)
                with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=fd)
            path.write_text(original); (output/'KernelMeasurement.obj').write_bytes(b'')
            with self.assertRaises(ValueError):loader_measurement_metadata(build,generic,fd=fd)
            (output/'KernelMeasurement.obj').unlink()
            with self.assertRaises(FileNotFoundError):loader_measurement_metadata(build,generic,fd=fd)
