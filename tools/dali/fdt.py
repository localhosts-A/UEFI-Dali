#!/usr/bin/env python3
"""Strict read-only FDT inspection and arena validation for dali UEFI artifacts."""
import struct

ARENAS = [("FD", 0xC1000000, 0x4000000), ("stack", 0xC5000000, 0x40000),
          ("FDT copy", 0xC5080000, 0x100000), ("heap", 0xC5200000, 0x8000000)]


class Fdt:
    def __init__(self, data):
        if len(data) < 40:
            raise ValueError("truncated FDT header")
        magic, total, off, strings_off, reserve, version, _, _, strings_size, size = struct.unpack_from(">10I", data)
        if magic != 0xD00DFEED or total > len(data) or version < 17:
            raise ValueError("unsupported FDT header")
        if off + size > total or strings_off + strings_size > total:
            raise ValueError("FDT block outside total size")
        strings = data[strings_off:strings_off + strings_size]
        self.nodes, self.reserved, stack = {}, [], []
        while off < total:
            token = struct.unpack_from(">I", data, off)[0]
            off += 4
            if token == 1:
                end = data.index(0, off, total)
                stack.append(data[off:end].decode())
                off = (end + 4) & ~3
                self.nodes["/" + "/".join(stack[1:])] = {}
            elif token == 2:
                stack.pop()
            elif token == 3:
                count, nameoff = struct.unpack_from(">II", data, off)
                off += 8
                if count > total - off or nameoff >= len(strings):
                    raise ValueError("FDT property out of bounds")
                name = strings[nameoff:strings.index(0, nameoff)].decode()
                self.nodes["/" + "/".join(stack[1:])][name] = data[off:off + count]
                off = (off + count + 3) & ~3
            elif token == 4:
                pass
            elif token == 9:
                break
            else:
                raise ValueError("invalid FDT structure token")
        else:
            raise ValueError("missing FDT end")
        while reserve + 16 <= total:
            base, length = struct.unpack_from(">QQ", data, reserve)
            reserve += 16
            if base == length == 0:
                break
            self.reserved.append((base, length))

    def reg(self, path):
        parent = self.nodes[path.rsplit("/", 1)[0] or "/"]
        ac = int.from_bytes(parent.get("#address-cells", b"\0\0\0\2"), "big")
        sc = int.from_bytes(parent.get("#size-cells", b"\0\0\0\1"), "big")
        raw = self.nodes[path].get("reg", b"")
        step = (ac + sc) * 4
        if ac not in (1, 2) or sc not in (1, 2) or len(raw) % step:
            raise ValueError("invalid reg in " + path)
        return [(int.from_bytes(raw[i:i + ac * 4], "big"),
                 int.from_bytes(raw[i + ac * 4:i + step], "big")) for i in range(0, len(raw), step)]

    def validate(self):
        if b"mediatek,MT6991" not in self.nodes["/"].get("compatible", b"").split(b"\0"):
            raise ValueError("not an MT6991 FDT")
        memory = self.reg("/memory")
        blocked = [("memreserve", base, size) for base, size in self.reserved]
        for path, properties in self.nodes.items():
            if path.startswith("/reserved-memory/") or b"pkvm,protected-region" in properties.get("compatible", b"").split(b"\0"):
                blocked += [(path, base, size) for base, size in self.reg(path)]
        chosen = self.nodes.get("/chosen", {})
        if "linux,initrd-start" in chosen and "linux,initrd-end" in chosen:
            start = int.from_bytes(chosen["linux,initrd-start"], "big")
            end = int.from_bytes(chosen["linux,initrd-end"], "big")
            if end < start:
                raise ValueError("invalid initrd extent")
            blocked.append(("LK initrd", start, end - start))
        for name, base, size in ARENAS:
            if not any(lo <= base and base + size <= lo + count for lo, count in memory):
                raise ValueError(name + " is outside DRAM")
            for source, lo, count in blocked:
                if count and base < lo + count and lo < base + size:
                    raise ValueError(name + " overlaps " + source)
        return {"arenas": ARENAS, "reserved_ranges": len(blocked)}

    def validate_io(self):
        """Verify reserved framebuffer, MMIO map and watchdog handoff contract."""
        self.validate()
        framebuffer = (0xFC990000, 0x3660000)
        chosen = self.nodes.get('/chosen', {})
        expected = {'atag,videolfb-fb_base_l': framebuffer[0],
                    'atag,videolfb-fb_base_h': 0, 'atag,videolfb-vramSize': framebuffer[1]}
        if any(chosen.get(k) != struct.pack('>I', v) for k, v in expected.items()):
            raise ValueError('LK framebuffer tag mismatch')
        if self.reg('/reserved-memory/mblock-15-framebuffer') != [framebuffer]:
            raise ValueError('framebuffer reservation mismatch')
        lo, count = framebuffer
        if not any(base <= lo and lo + count <= base + size for base, size in self.reg('/memory')):
            raise ValueError('framebuffer outside DRAM')
        for path, properties in self.nodes.items():
            if b'pkvm,protected-region' in properties.get('compatible', b'').split(b'\0'):
                if any(size and lo < base + size and base < lo + count for base, size in self.reg(path)):
                    raise ValueError('framebuffer overlaps protected region')
        regions = [('/soc/pinctrl@1002d000', 0x1002D000), ('/watchdog@1c010000', 0x1C010000)]
        regions += [(f'/soc/dsi@{base:x}', base) for base in (0x32490000, 0x324A0000, 0x324B0000)]
        regions += [(f'/soc/disp-ovl{side}-exdma{unit}@{base:x}', base)
                    for side in range(2) for unit in (0, 2, 3, 4, 5, 6, 7, 8, 9)
                    if not (side == 1 and unit == 0)
                    for base in [0x32830000 + side * 0x400000 + unit * 0x10000]]
        for path, base in regions:
            if path not in self.nodes or self.reg(path)[0] != (base, 0x1000):
                raise ValueError('MMIO probe map mismatch: ' + path)
        watchdog = self.nodes['/watchdog@1c010000'].get('compatible', b'').split(b'\0')
        if not all(c in watchdog for c in (b'mediatek,mt6991-wdt', b'mediatek,mt6589-wdt')):
            raise ValueError('watchdog compatible mismatch')
        mutex_path = '/soc/disp1-mutex0@32020000'
        if mutex_path not in self.nodes or self.reg(mutex_path) != [(base, 0x1000) for base in
                (0x32020000, 0x32420000, 0x32820000, 0x32c20000)]:
            raise ValueError('display mutex map mismatch')
        key = self.nodes.get('/gpio-keys/volumedown', {})
        controller = self.nodes['/soc/pinctrl@1002d000']
        pins = controller.get('phandle', controller.get('linux,phandle'))
        if pins is None or key.get('gpios') != pins + struct.pack('>II', 60, 1) or key.get('linux,code') != struct.pack('>I', 115):
            raise ValueError('GPIO60 key binding mismatch')
        spmi = self.nodes.get('/spmi@1c01a000', {})
        expected_spmi = [(0x1c01a000, 0x8ff), (0x1c01c000, 0x100),
                         (0x1c018000, 0x8ff), (0x1c01c800, 0x100)]
        if (b'mediatek,mt6991-spmi' not in spmi.get('compatible', b'').split(b'\0') or
                self.reg('/spmi@1c01a000') != expected_spmi or
                spmi.get('reg-names') != b'pmif\0spmimst\0pmif-p\0spmimst-p\0' or
                len(spmi.get('spmi-dev-mask', b'')) != 4 or
                int.from_bytes(spmi['spmi-dev-mask'], 'big') & (1 << 4)):
            raise ValueError('PMIF map or SID4 routing mismatch')
        pmic = self.nodes.get('/spmi@1c01a000/pmic@4', {})
        keys_path = '/spmi@1c01a000/pmic@4/mt6363keys'
        keys = self.nodes.get(keys_path, {})
        if (pmic.get('reg') != struct.pack('>II', 4, 0) or
                b'mediatek,mt6363' not in pmic.get('compatible', b'').split(b'\0') or
                b'mediatek,mt6363-keys' not in keys.get('compatible', b'').split(b'\0') or
                self.nodes.get(keys_path + '/power', {}).get('linux,keycodes') != struct.pack('>I', 116) or
                self.nodes.get(keys_path + '/home', {}).get('linux,keycodes') != struct.pack('>I', 114)):
            raise ValueError('MT6363 key binding mismatch')
        return {'framebuffer': framebuffer, 'mmio_probe_nodes': len(regions) + 2, 'gpio_key': 60, 'pmic_key_sid': 4}
