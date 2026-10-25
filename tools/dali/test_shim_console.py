"""Execute the complete production shim against retained/invalid ramoops heads."""
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest


class ShimConsoleTests(unittest.TestCase):
    def test_real_shim_preserves_ring_fdt_and_relocates_payload(self):
        if not all(shutil.which(p) for p in ('clang', 'ld.lld', 'llvm-objcopy', 'qemu-system-aarch64')):
            self.skipTest('AArch64 toolchain and QEMU required')
        shim_source = Path(__file__).resolve().parents[2] / 'BootShim/DaliShim.S'
        signature, capacity = 0x43474244, 0x200000 - 12
        marker = b'DALI: UEFI relocation shim entered\n'
        cases = [(signature, 0, 0), (signature, 17, 17), (signature, 7, capacity),
                 (signature, capacity-4, capacity), (0, 22, 22),
                 (signature, capacity, 0), (signature, 0, capacity+1)]
        with tempfile.TemporaryDirectory(prefix='dali-shim-') as temporary:
            work = Path(temporary)
            def compile_binary(source, stem, address, *options):
                obj, elf, binary = (work / (stem + suffix) for suffix in ('.o', '.elf', '.bin'))
                subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(source), '-o', str(obj), *options], check=True)
                subprocess.run(['ld.lld', '-m', 'aarch64elf', f'-Ttext={address:#x}', '-e', '_start', str(obj), '-o', str(elf)], check=True)
                subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(binary)], check=True)
                return binary.read_bytes()
            (work/'entry.S').write_text('.text\n.global _start\n_start:\nldr x0, =0x48300000\nldr x1, =0x48000000\nbr x1\n.ltorg\n')
            compile_binary(work/'entry.S', 'entry', 0x47000000)
            (work/'payload.S').write_text('.text\n.global _start\n_start:\nldr x1, =0x48201000\nstr x0, [x1]\ndsb sy\n1: b 1b\n.ltorg\n.balign 4096\n')
            payload = compile_binary(work/'payload.S', 'payload', 0x50000000)
            shim = compile_binary(shim_source, 'shim', 0x48000000, '-DFD_BASE=0x50000000', '-DFD_SIZE=0x1000')
            self.assertEqual(len(shim), 4096)
            self.assertEqual(len(payload), 4096)
            self.assertEqual(shim[56:60], b'ARM\x64')
            self.assertEqual(struct.unpack_from('<Q', shim, 16)[0], 8192)
            (work/'image.bin').write_bytes(shim + payload)
            for sig, start, size in cases:
                with self.subTest(head=(sig, start, size)):
                    before = bytearray(b'\x5a' * (0x200000 + 32))
                    struct.pack_into('<III', before, 16, sig, start, size)
                    (work/'ring.bin').write_bytes(before)
                    expected = before.copy()
                    if sig != signature or start >= capacity or size > capacity:
                        start = size = 0
                    for char in marker:
                        expected[28 + start] = char
                        start = (start + 1) % capacity
                        size = min(size + 1, capacity)
                    struct.pack_into('<III', expected, 16, signature, start, size)
                    qmp_path = work/'qmp'
                    qmp_path.unlink(missing_ok=True)
                    command = ['qemu-system-aarch64', '-M', 'virt,virtualization=on', '-cpu', 'cortex-a76',
                               '-m', '4G', '-smp', '1', '-display', 'none', '-serial', 'none', '-monitor', 'none', '-S',
                               '-qmp', f'unix:{qmp_path},server=on,wait=off',
                               '-device', f'loader,file={work / "entry.bin"},addr=0x47000000,cpu-num=0,force-raw=on',
                               '-device', f'loader,file={work / "image.bin"},addr=0x48000000,force-raw=on',
                               '-device', f'loader,file={work / "ring.bin"},addr=0xaec0eff0,force-raw=on']
                    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    try:
                        deadline = time.monotonic() + 5
                        while not qmp_path.exists() and process.poll() is None and time.monotonic() < deadline:
                            time.sleep(0.01)
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.settimeout(5)
                            connection.connect(str(qmp_path))
                            stream = connection.makefile('rwb')
                            stream.readline()
                            def qmp(name, **arguments):
                                stream.write(json.dumps({'execute': name, 'arguments': arguments}).encode() + b'\n')
                                stream.flush()
                                while True:
                                    response = json.loads(stream.readline())
                                    if 'return' in response:
                                        return response['return']
                                    if 'error' in response:
                                        self.fail(str(response['error']))
                            qmp('qmp_capabilities')
                            qmp('cont')
                            # Wait for the payload's actual ABI store after
                            # relocation; a fixed host sleep races TCG startup.
                            deadline = time.monotonic() + 5
                            while True:
                                result = qmp('human-monitor-command', **{'command-line':
                                    f'pmemsave 0x48201000 8 "{work / "completion"}"'})
                                self.assertFalse(result, result)
                                if (work/'completion').read_bytes() == struct.pack('<Q', 0x48300000):
                                    break
                                self.assertLess(time.monotonic(), deadline, 'payload did not complete relocation')
                                time.sleep(0.01)
                            qmp('stop')
                            for address, length, name in ((0xaec0eff0, len(before), 'actual'),
                                                          (0x50000000, 4096, 'relocated'), (0x48201000, 8, 'abi')):
                                result = qmp('human-monitor-command', **{'command-line':
                                    f'pmemsave {address:#x} {length:#x} "{work / name}"'})
                                self.assertFalse(result, result)
                            qmp('quit')
                        process.wait(timeout=5)
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait(timeout=5)
                    self.assertEqual((work/'actual').read_bytes(), expected)
                    self.assertEqual((work/'relocated').read_bytes(), payload)
                    self.assertEqual(struct.unpack('<Q', (work/'abi').read_bytes())[0], 0x48300000)
