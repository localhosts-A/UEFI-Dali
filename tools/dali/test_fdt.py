#!/usr/bin/env python3
"""Host preflight tests: foreign platforms and occupied memory must fail closed."""
import subprocess
import unittest
from fdt import Fdt


def tree(extra="", reserved="", compatible="mediatek,MT6991"):
    dts = f'''/dts-v1/;
    / {{ compatible = "{compatible}"; #address-cells = <2>; #size-cells = <2>;
      memory {{ device_type = "memory"; reg = <0 0x80000000 3 0>; }};
      reserved-memory {{ #address-cells = <2>; #size-cells = <2>; ranges; {reserved} }};
      {extra}
    }};'''
    return subprocess.check_output(["dtc", "-q", "-I", "dts", "-O", "dtb"], input=dts.encode())


def io_tree():
    nodes = ['pinctrl@1002d000 { reg = <0 0x1002d000 0 0x1000>; phandle = <42>; };']
    nodes += ['disp1-mutex0@32020000 { reg = <0 0x32020000 0 0x1000>, <0 0x32420000 0 0x1000>, <0 0x32820000 0 0x1000>, <0 0x32c20000 0 0x1000>; };']
    nodes += [f'dsi@{base:x} {{ reg = <0 0x{base:x} 0 0x1000>; }};'
              for base in (0x32490000, 0x324a0000, 0x324b0000)]
    for side in range(2):
        for unit in (0, 2, 3, 4, 5, 6, 7, 8, 9):
            if side == 1 and unit == 0:
                continue
            base = 0x32830000 + side * 0x400000 + unit * 0x10000
            nodes.append(f'disp-ovl{side}-exdma{unit}@{base:x} {{ reg = <0 0x{base:x} 0 0x1000>; }};')
    extra = '''chosen { atag,videolfb-fb_base_l = <0xfc990000>; atag,videolfb-fb_base_h = <0>;
                       atag,videolfb-vramSize = <0x3660000>; };
               gpio-keys { volumedown { gpios = <42 60 1>; linux,code = <115>; }; };
               soc { #address-cells = <2>; #size-cells = <2>; ''' + ''.join(nodes) + ' };'
    return Fdt(tree(extra=extra, reserved='mblock-15-framebuffer { reg = <0 0xfc990000 0 0x3660000>; };'))


class PreflightTests(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(len(Fdt(tree()).validate()["arenas"]), 4)

    def test_heap_carveout_rejected(self):
        with self.assertRaisesRegex(ValueError, "overlaps"):
            Fdt(tree(reserved='hole@c2000000 { reg = <0 0xc2000000 0 0x1000>; no-map; };')).validate()

    def test_protected_region_rejected(self):
        with self.assertRaisesRegex(ValueError, "overlaps"):
            Fdt(tree(extra='protected@c1000000 { compatible = "pkvm,protected-region"; reg = <0 0xc1000000 0 0x400000>; };')).validate()

    def test_initrd_collision_rejected(self):
        with self.assertRaisesRegex(ValueError, "LK initrd"):
            Fdt(tree(extra='chosen { linux,initrd-start = <0xc1000000>; linux,initrd-end = <0xc1500000>; };')).validate()

    def test_foreign_platform_rejected(self):
        with self.assertRaisesRegex(ValueError, "not an MT6991"):
            Fdt(tree(compatible="qcom,sm7325")).validate()

    def test_truncated_header_rejected(self):
        with self.assertRaisesRegex(ValueError, "truncated"):
            Fdt(b"\xd0\x0d\xfe\xed")

    def test_truncated_blob_rejected(self):
        with self.assertRaisesRegex(ValueError, "header"):
            Fdt(tree()[:-16])

    def test_io_layout(self):
        self.assertEqual(io_tree().validate_io()['mmio_probe_nodes'], 22)

    def test_changed_framebuffer_tag_rejected(self):
        f = io_tree()
        f.nodes['/chosen']['atag,videolfb-vramSize'] = bytes.fromhex('ffffffff')
        with self.assertRaisesRegex(ValueError, 'framebuffer tag'):
            f.validate_io()

    def test_changed_mmio_map_rejected(self):
        f = io_tree()
        f.nodes['/soc/pinctrl@1002d000']['reg'] = bytes.fromhex('000000001002e0000000000000001000')
        with self.assertRaisesRegex(ValueError, 'MMIO probe map'):
            f.validate_io()

    def test_changed_gpio_binding_rejected(self):
        f = io_tree()
        f.nodes['/gpio-keys/volumedown']['gpios'] = bytes.fromhex('0000002a0000003d00000001')
        with self.assertRaisesRegex(ValueError, 'GPIO60'):
            f.validate_io()

    def test_changed_mutex_map_rejected(self):
        f = io_tree()
        f.nodes['/soc/disp1-mutex0@32020000']['reg'] = bytes(64)
        with self.assertRaisesRegex(ValueError, 'display mutex map'):
            f.validate_io()

    def test_protected_framebuffer_rejected(self):
        f = io_tree()
        f.nodes['/protected'] = {'compatible': b'pkvm,protected-region\0',
                                 'reg': bytes.fromhex('00000000fc9900000000000000001000')}
        with self.assertRaisesRegex(ValueError, 'framebuffer overlaps protected'):
            f.validate_io()


if __name__ == "__main__":
    unittest.main()
