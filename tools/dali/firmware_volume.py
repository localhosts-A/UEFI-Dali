#!/usr/bin/env python3
"""Extract one PE32 module from the actual Dali FV/LZMA firmware container.

This is a bounded build-product verifier, not a general firmware parser. Unknown
compression/encapsulation refuses qualification instead of searching for token
strings in an unrelated output directory.
"""
import lzma
import struct
import uuid

LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le
BDS_GUID = uuid.UUID('e7437b10-23c2-4568-9b18-9b1b7a3e0925').bytes_le
LIMIT = 128 * 1024 * 1024


def extract_module(fd, file_guid):
    if len(file_guid) != 16:
        raise ValueError('firmware file GUID must be 16 bytes')
    found = []

    def sections(data, selected, depth):
        if depth > 8:
            raise ValueError('firmware encapsulation depth exceeded')
        at = 0
        while at + 4 <= len(data):
            if all(byte == 0xff for byte in data[at:at + 4]):
                break
            length = int.from_bytes(data[at:at + 3], 'little')
            kind, head = data[at + 3], 4
            if length == 0xffffff:
                if at + 8 > len(data):
                    raise ValueError('truncated extended section')
                length, head = struct.unpack_from('<I', data, at + 4)[0], 8
            if length < head or length > len(data) - at:
                raise ValueError('section exceeds firmware file')
            body = data[at + head:at + length]
            if kind == 0x17:  # EFI_SECTION_FIRMWARE_VOLUME_IMAGE
                volume(body, depth + 1)
            elif kind == 2:  # EFI_SECTION_GUID_DEFINED
                if len(body) < 20:
                    raise ValueError('truncated GUID section')
                guid = body[:16]
                offset, attributes = struct.unpack_from('<HH', body, 16)
                if guid != LZMA_GUID or attributes != 1 or not head + 20 <= offset <= length:
                    raise ValueError('unsupported guided firmware section')
                decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
                try:
                    unpacked = decoder.decompress(data[at + offset:at + length], max_length=LIMIT + 1)
                except lzma.LZMAError as error:
                    raise ValueError('invalid LZMA firmware section') from error
                if len(unpacked) > LIMIT or not decoder.eof or decoder.unused_data:
                    raise ValueError('unbounded/truncated LZMA firmware section')
                sections(unpacked, selected, depth + 1)
            elif kind == 0x10 and selected:  # EFI_SECTION_PE32
                if not body.startswith(b'MZ'):
                    raise ValueError('selected section is not PE32')
                found.append(body)
            elif kind == 1:
                raise ValueError('unsupported standard firmware compression')
            at = (at + length + 3) & ~3

    def volume(data, depth):
        if depth > 8 or len(data) < 56 or data[40:44] != b'_FVH':
            raise ValueError('invalid firmware volume header')
        length = struct.unpack_from('<Q', data, 32)[0]
        head = struct.unpack_from('<H', data, 48)[0]
        attributes = struct.unpack_from('<I', data, 44)[0]
        if not 56 <= head <= length <= len(data) or head % 2:
            raise ValueError('firmware volume bounds mismatch')
        if sum(struct.unpack('<' + 'H' * (head // 2), data[:head])) & 0xffff:
            raise ValueError('firmware volume header checksum mismatch')
        at = (head + 7) & ~7
        while at + 24 <= length:
            raw = data[at:at + 24]
            if raw == b'\xff' * 24:
                break
            size, fh = int.from_bytes(raw[20:23], 'little'), 24
            if raw[19] & 1:
                if size != 0xffffff or at + 32 > length:
                    raise ValueError('invalid extended firmware file')
                size, fh = struct.unpack_from('<Q', data, at + 24)[0], 32
            if size < fh or size > length - at:
                raise ValueError('firmware file exceeds volume')
            state = raw[23] ^ (0xff if attributes & 0x800 else 0)
            highest = 1 << (state.bit_length() - 1) if state else 0
            if highest in (4, 8) and raw[18] != 0xf0:  # valid/update, not PAD
                sections(data[at + fh:at + size], raw[:16] == file_guid, depth + 1)
            at = (at + size + 7) & ~7

    if len(fd) > LIMIT:
        raise ValueError('firmware exceeds verifier bound')
    volume(fd, 0)
    if len(found) != 1:
        raise ValueError('final FD must contain exactly one selected PE32 module')
    return found[0]


def extract_bds(fd):
    return extract_module(fd, BDS_GUID)
