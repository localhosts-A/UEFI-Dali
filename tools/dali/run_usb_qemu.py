#!/usr/bin/env python3
"""Real xHCI/hub/HID/BOT/GPT/FAT32 test. Creates new image files; no device access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import uuid
import zlib

MIB = 1024 * 1024
NAMES = ('XhciDxe', 'UsbBusDxe', 'UsbKbDxe', 'UsbMouseDxe', 'UsbMassStorageDxe',
         'DiskIoDxe', 'PartitionDxe', 'Fat', 'UsbQemuTest')


def call(*cmd):
    subprocess.run(list(map(str, cmd)), check=True, capture_output=True, timeout=60)


def disk(partition, target):
    """One real GPT partition, with valid primary and backup headers/tables."""
    sectors, start, length = 128 * MIB // 512, 2048, partition.stat().st_size // 512
    entries = bytearray(128 * 128)
    entries[:16] = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    entries[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', entries, 32, start, start + length - 1, 0)
    name = 'DALI USB QEMU'.encode('utf-16le')
    entries[56:56 + len(name)] = name
    guid = uuid.uuid4().bytes_le

    def header(here, other, table):
        value = bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII', value, 0, b'EFI PART', 0x10000, 92, 0, 0,
                         here, other, 34, sectors - 34, guid, table, 128, 128, zlib.crc32(entries))
        struct.pack_into('<I', value, 16, zlib.crc32(value[:92]))
        return value

    with target.open('xb') as out:
        out.truncate(sectors * 512)
        mbr = bytearray(512)
        mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\0\2\0', 0xee, b'\xff\xff\xff', 1, sectors - 1)
        mbr[510:512] = b'\x55\xaa'
        for offset, data in ((0, mbr), (512, header(1, sectors - 1, 2)), (1024, entries),
                             ((sectors - 33) * 512, entries), ((sectors - 1) * 512, header(sectors - 1, 1, sectors - 33))):
            out.seek(offset); out.write(data)
        out.seek(start * 512)
        with partition.open('rb') as source:
            shutil.copyfileobj(source, out)


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(5)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile('rwb', buffering=0)
        json.loads(self.stream.readline())
        self.execute('qmp_capabilities')

    def execute(self, command, arguments=None):
        message = {'execute': command}
        if arguments is not None:
            message['arguments'] = arguments
        self.stream.write(json.dumps(message).encode() + b'\n')
        while True:
            reply = json.loads(self.stream.readline())
            if 'error' in reply:
                raise RuntimeError(reply)
            if 'return' in reply:
                return reply['return']

    def close(self):
        self.stream.close(); self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, default=Path('/usr/share/edk2/aarch64/QEMU_EFI.fd'))
    parser.add_argument('--vars', type=Path, default=Path('/usr/share/edk2/aarch64/QEMU_VARS.fd'))
    args = parser.parse_args()
    for tool in ('qemu-system-aarch64', 'mkfs.fat', 'mcopy', 'mmd', 'mdir'):
        if not shutil.which(tool):
            parser.error(f'missing {tool}')
    manifest = json.loads(args.build.read_text())
    if set(manifest['artifacts']) != set(NAMES):
        parser.error('expected exact source module manifest')
    run = args.output.resolve() / ('run-' + uuid.uuid4().hex)
    run.mkdir(parents=True)
    for name, item in manifest['artifacts'].items():
        if hashlib.sha256(Path(item['path']).read_bytes()).hexdigest() != item['sha256']:
            parser.error(f'changed build artifact: {name}')
    boot = run / 'boot.fat'; store = run / 'store.fat'
    for image, label in ((boot, 'DALIBOOT'), (store, 'DALIUSB')):
        with image.open('xb') as f:
            f.truncate(96 * MIB)
        call('mkfs.fat', '-F', '32', '-n', label, image)
    call('mmd', '-i', boot, '::/EFI', '::/EFI/BOOT', '::/EFI/DALI')
    for name, item in manifest['artifacts'].items():
        call('mcopy', '-i', boot, item['path'], '::/EFI/BOOT/BOOTAA64.EFI' if name == 'UsbQemuTest' else f'::/EFI/DALI/{name}.efi')
    marker = run / 'USBTEST.TAG'; marker.write_bytes(b'DALI USB QEMU ONLY\n')
    call('mcopy', '-i', store, marker, '::/USBTEST.TAG')
    boot_disk, storage = run / 'boot.img', run / 'usb.img'
    disk(boot, boot_disk); disk(store, storage)
    vars_template_sha256 = hashlib.sha256(args.vars.read_bytes()).hexdigest()
    variables = run / 'vars.fd'; shutil.copyfile(args.vars, variables)
    # Keep the Unix socket path below the platform's 108-byte limit.
    qmp_path = Path('/tmp') / ('dali-usb-' + uuid.uuid4().hex + '.sock')
    serial = run / 'serial.log'
    command = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a72', '-m', '1024', '-smp', '1',
               '-drive', f'if=pflash,format=raw,readonly=on,file={args.firmware.resolve()}',
               '-drive', f'if=pflash,format=raw,file={variables}',
               '-drive', f'if=none,id=esp,format=raw,readonly=on,file={boot_disk}',
               '-device', 'virtio-blk-pci,drive=esp,bootindex=0',
               '-device', 'qemu-xhci,id=xhci,p2=4,p3=0', '-device', 'usb-hub,id=hub,bus=xhci.0,port=1',
               '-device', 'usb-kbd,bus=xhci.0,port=1.1', '-device', 'usb-mouse,bus=xhci.0,port=1.2',
               '-drive', f'if=none,id=store,format=raw,file={storage}',
               '-device', 'usb-storage,drive=store,bus=xhci.0,port=1.3,serial=DALIUSBTEST',
               '-device', 'ramfb', '-nic', 'none', '-display', 'none', '-monitor', 'none', '-no-reboot',
               '-serial', f'file:{serial}', '-qmp', f'unix:{qmp_path},server=on,wait=off']
    qmp = None; sent = released = False
    with (run / 'qemu.log').open('wb') as output:
        proc = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 120
            while proc.poll() is None and time.monotonic() < deadline:
                if qmp is None and qmp_path.exists():
                    qmp = Qmp(qmp_path)
                log = serial.read_text(errors='replace') if serial.exists() else ''
                if 'DALI_USB_HID_READY' in log and not sent and qmp:
                    qmp.execute('send-key', {'keys': [{'type': 'qcode', 'data': 'a'}], 'hold-time': 200})
                    qmp.execute('input-send-event', {'events': [
                        {'type': 'rel', 'data': {'axis': 'x', 'value': 12}}, {'type': 'rel', 'data': {'axis': 'y', 'value': -5}},
                        {'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
                    sent = True
                if 'DALI_USB_MOUSE_DOWN' in log and not released and qmp:
                    qmp.execute('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
                    released = True
                time.sleep(0.05)
            if proc.poll() is None:
                raise TimeoutError('USB QEMU gate timed out')
            if proc.returncode != 0:
                raise RuntimeError(f'QEMU exit={proc.returncode}; inspect {run}')
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=10)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=10)
            if qmp: qmp.close()
            qmp_path.unlink(missing_ok=True)
    log = serial.read_text(errors='replace')
    required = ('DALI_USB_HID_READY', 'DALI_USB_MOUSE_DOWN', 'DALI_USB_HID_PASS',
                'DALI_USB_FAT_REMOUNT_PASS', 'DALI_USB_SOURCE_BINDINGS_PASS', 'DALI_USB_QEMU_PASS')
    positions = [log.find(marker) for marker in required]
    if not sent or not released or any(log.count(marker) != 1 for marker in required) or \
            positions != sorted(positions) or 'DALI_USB_FAIL' in log:
        raise RuntimeError(f'incomplete gate: inspect {serial}')
    readback = run / 'readback.bin'
    call('mcopy', '-i', f'{storage}@@1048576', '::/UEFIUSB.DAT', readback)
    expected = bytearray((i * 37 + 11) & 255 for i in range(32768))
    expected[4096:5120] = bytes((i * 19 + 7) & 255 for i in range(1024))
    digest = hashlib.sha256(readback.read_bytes()).hexdigest()
    if digest != hashlib.sha256(expected).hexdigest():
        raise RuntimeError('post-shutdown FAT32 SHA256 mismatch')
    listing = subprocess.check_output(['mdir', '-b', '-i', f'{storage}@@1048576', '::/'], text=True)
    files = {item.rsplit('/', 1)[-1].upper() for item in listing.splitlines()}
    if 'UEFIUSB.DAT' not in files or {'DELETE.ME', 'UEFIUSB.BIN'} & files:
        raise RuntimeError('rename/deletion did not persist')
    result = {'scope': 'actual source xHCI/USB bus/HID/BOT/GPT/FAT32 under external QEMU UEFI platform; no MT6991/XNU proof',
              'passed': True, 'hardware_qualified': False, 'qemu': subprocess.check_output(['qemu-system-aarch64', '--version'], text=True).splitlines()[0],
              'firmware_sha256': hashlib.sha256(args.firmware.read_bytes()).hexdigest(), 'source_build': manifest,
              'vars_template_sha256': vars_template_sha256,
              'readback_sha256': digest, 'command': command, 'serial_log': str(serial), 'run_directory': str(run),
              'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'boot_image_sha256': hashlib.sha256(boot_disk.read_bytes()).hexdigest(),
              'usb_image_sha256': hashlib.sha256(storage.read_bytes()).hexdigest(),
              'fat_directory': sorted(files),
              'hub_scope': 'QEMU usb-hub full-speed, xHCI USB2 ports only; no physical high-speed hub qualification'}
    (run / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
