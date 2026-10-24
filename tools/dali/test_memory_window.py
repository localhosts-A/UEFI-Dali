"""Execute the firmware's C range selector, including hostile reservations."""
import ctypes
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest
from test_fdt import tree


class Range(ctypes.Structure):
    _fields_ = [("base", ctypes.c_uint64), ("end", ctypes.c_uint64)]


class MemoryWindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler required")
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        header = Path(__file__).resolve().parents[2] / "Silicon/MediaTek/MT6991Pkg/Library/PlatformLib/MemoryWindow.h"
        (base / "window.c").write_text('''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <libfdt.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef intptr_t INTN;
typedef size_t UINTN;
typedef bool BOOLEAN;
#define VOID void
#define STATIC static
#define CONST const
#define TRUE true
#define FALSE false
#define MAX_UINT64 UINT64_MAX
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define Fdt32ToCpu fdt32_to_cpu
static size_t parent_calls;
static int counted_parent(const void *fdt, int node) {
  parent_calls++;
  return fdt_parent_offset(fdt, node);
}
#define FdtParentOffset counted_parent
#define FdtAddressCells fdt_address_cells
#define FdtSizeCells fdt_size_cells
#define FdtGetProp fdt_getprop
#define FdtCheckHeader fdt_check_header
#define FdtPathOffset fdt_path_offset
#define FdtTotalSize fdt_totalsize
#define FdtGetNumberOfReserveMapEntries fdt_num_mem_rsv
#define FdtGetReserveMapEntry fdt_get_mem_rsv
#define FdtNextNode fdt_next_node
#define FdtStringListSearch fdt_stringlist_search
#include "MemoryWindowFdt.h"
bool choose(const DALI_RAM_RANGE *banks, size_t nb, const DALI_RAM_RANGE *blocked,
            size_t nx, uint64_t *base, uint64_t *size) {
  return DaliChooseWindow(banks, nb, blocked, nx, base, size);
}
bool add(uint64_t base, uint64_t size, size_t capacity) {
  DALI_RAM_RANGE ranges[1]; size_t count = 0;
  return DaliAddRange(ranges, &count, capacity, base, size);
}
bool select_fdt(const void *fdt, uint64_t address, const DALI_RAM_RANGE *fixed,
                size_t count, uint64_t *base, uint64_t *size) {
  size_t exclusions = 0;
  parent_calls = 0;
  return DaliWindowFromFdt(fdt, address, fixed, count, base, size, &exclusions);
}
size_t get_parent_calls(void) { return parent_calls; }
''')
        subprocess.run([compiler, "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(header.parent), str(base / "window.c"), "-lfdt", "-o", str(base / "window.so")], check=True)
        cls.lib = ctypes.CDLL(str(base / "window.so"))
        cls.lib.choose.argtypes = [ctypes.POINTER(Range), ctypes.c_size_t, ctypes.POINTER(Range),
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.choose.restype = ctypes.c_bool
        cls.lib.add.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_size_t]
        cls.lib.add.restype = ctypes.c_bool
        cls.lib.select_fdt.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(Range), ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.select_fdt.restype = ctypes.c_bool
        cls.lib.get_parent_calls.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def choose(self, banks, blocked):
        base, size = ctypes.c_uint64(), ctypes.c_uint64()
        ok = self.lib.choose((Range * len(banks))(*(Range(*r) for r in banks)), len(banks),
                             (Range * len(blocked))(*(Range(*r) for r in blocked)), len(blocked),
                             ctypes.byref(base), ctypes.byref(size))
        self.assertEqual(ok, size.value != 0)
        return base.value, size.value

    def test_insufficient_or_separate_banks_fail_closed(self):
        self.assertEqual(self.choose([(0xc5200000, 0xcd200000)], []), (0, 0))
        self.assertEqual(self.choose([(0x80000000, 0x90000000), (0xa0000000, 0xb0000000)], []), (0, 0))

    def test_clips_to_below_four_gib(self):
        self.assertEqual(self.choose([(0xc0000000, 0x380000000)], []), (0xc0000000, 0x40000000))
        self.assertEqual(self.choose([(0x100000000, 0x380000000)], []), (0, 0))

    def test_unaligned_overlapping_and_unsorted_reservations(self):
        blocked = [(0xd0001000, 0xe0100000), (0xc0000000, 0xd1000000), (0x80001000, 0x81000001)]
        self.assertEqual(self.choose([(0x80000000, 0x100000000)], blocked), (0x81200000, 0x3ee00000))

    def test_overflow_and_capacity_are_rejected(self):
        self.assertFalse(self.lib.add(0xfffffffffffff000, 0x2000, 1))
        self.assertFalse(self.lib.add(0x80000000, 0x2000, 0))
        self.assertTrue(self.lib.add(0x80000000, 0x2000, 1))

    def select_fdt(self, data, address=0x90000000):
        base, size = ctypes.c_uint64(), ctypes.c_uint64()
        fixed = (Range * 2)(Range(0x80000000, 0xa0000000), Range(0xd0000000, 0x100000000))
        ok = self.lib.select_fdt(ctypes.create_string_buffer(data), address, fixed, 2,
                                 ctypes.byref(base), ctypes.byref(size))
        return (base.value, size.value) if ok else None

    def test_actual_fdt_parser_excludes_each_reservation_source(self):
        self.assertEqual(self.select_fdt(tree()), (0xa0000000, 0x30000000))
        self.assertIsNone(self.select_fdt(tree(reserved='hole { reg = <0 0xb0000000 0 0x10000000>; };')))
        self.assertIsNone(self.select_fdt(tree(reserved='container { #address-cells = <2>; #size-cells = <2>; ranges; child { reg = <0 0xb0000000 0 0x10000000>; }; };')))
        self.assertIsNone(self.select_fdt(tree(extra='protected { compatible = "pkvm,protected-region"; reg = <0 0xb0000000 0 0x10000000>; };')))
        self.assertIsNone(self.select_fdt(tree(extra='chosen { linux,initrd-start = <0 0xb0000000>; linux,initrd-end = <0 0xc0000000>; };')))
        self.assertEqual(self.select_fdt(tree(), address=0xb8000000), (0xa0000000, 0x18000000))
        # A real FDT reserve-map entry, independent of /reserved-memory.
        dts = b'/dts-v1/; /memreserve/ 0xb0000000 0x10000000; / { #address-cells = <2>; #size-cells = <2>; memory { reg = <0 0x80000000 3 0>; }; reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges; }; };'
        data = subprocess.check_output(["dtc", "-q", "-I", "dts", "-O", "dtb"], input=dts)
        self.assertIsNone(self.select_fdt(data))

    def test_fdt_malformed_ranges_fail_closed(self):
        self.assertIsNone(self.select_fdt(tree(reserved='hole { reg = <0xffffffff 0xfffff000 0 0x2000>; };')))
        self.assertIsNone(self.select_fdt(tree(extra='chosen { linux,initrd-end = <0xc0000000>; };')))
        self.assertIsNone(self.select_fdt(tree(reserved='hole { reg = <0 0xb0000000 0>; };')))
        self.assertIsNone(self.select_fdt(tree(reserved='dynamic { size = <0 0x10000000>; alloc-ranges = <0 0xb0000000 0 0x10000000>; };')))
        self.assertIsNone(self.select_fdt(tree(reserved='dynamic { size = <0 0x20000000>; alloc-ranges = <0 0xb0000000 0 0x10000000>; };')))
        self.assertIsNone(self.select_fdt(tree(extra='protected { compatible = "pkvm,protected-region"; };')))
        self.assertIsNone(self.select_fdt(tree(reserved='dynamic { size = <0 0x1000>; alignment = <0 3>; };')))
        self.assertIsNone(self.select_fdt(tree(reserved='container { #address-cells = <2>; #size-cells = <2>; ranges = <0 0 0 0xb0000000 0 0x10000000>; child { reg = <0 0 0 0x10000000>; }; };')))

    def test_dynamic_pool_is_placed_before_choosing_xnu(self):
        self.assertEqual(self.select_fdt(tree(reserved='dynamic { size = <0 0x4000000>; alignment = <0 0x1000000>; alloc-ranges = <0 0xa0000000 0 0x30000000>; };')),
                         (0xa4000000, 0x2c000000))

    def test_reserved_walk_keeps_known_parents_without_root_rescans(self):
        reserved = "".join(f"hole{i} {{ reg = <0 0x81000000 0 0x1000>; }};" for i in range(80))
        self.assertEqual(self.select_fdt(tree(reserved=reserved)), (0xa0000000, 0x30000000))
        # Only the reserved-memory -> root chain needs parent lookup, regardless
        # of the number of static nodes or the repeated dynamic-pool pass.
        self.assertEqual(self.lib.get_parent_calls(), 2)

    def test_reserved_walk_uses_each_nested_parent_cell_width(self):
        reserved = ('container { #address-cells = <1>; #size-cells = <1>; ranges; '
                    'child { reg = <0xa0000000 0x10000000>; }; }; '
                    'sibling { reg = <0 0x81000000 0 0x1000>; };')
        self.assertEqual(self.select_fdt(tree(reserved=reserved)), (0xb0000000, 0x20000000))

    def test_reserved_walk_depth_limit_fails_closed(self):
        reserved = 'leaf { reg = <0 0x81000000 0 0x1000>; };'
        for index in range(64):
            reserved = f'level{index} {{ #address-cells = <2>; #size-cells = <2>; ranges; {reserved} }};'
        self.assertIsNone(self.select_fdt(tree(reserved=reserved)))

    def test_protected_translated_ancestor_still_fails_closed(self):
        extra = ('bus { #address-cells = <2>; #size-cells = <2>; '
                 'ranges = <0 0 0 0xb0000000 0 0x10000000>; '
                 'protected { compatible = "pkvm,protected-region"; reg = <0 0 0 0x1000>; }; };')
        self.assertIsNone(self.select_fdt(tree(extra=extra)))

    def test_randomized_against_interval_subtraction(self):
        rng = random.Random(6991)
        for _ in range(200):
            blocked = [(rng.randrange(0x80000000, 0x100000000), rng.randrange(0x1000, 0x18000000))
                       for _ in range(rng.randrange(7))]
            blocked = [(a, a + n) for a, n in blocked]
            free = [(0x80000000, 0x100000000)]
            for lo, hi in blocked:
                free = [piece for a, z in free for piece in
                        ([(a, z)] if hi <= a or lo >= z else [(a, min(z, lo)), (max(a, hi), z)])
                        if piece[1] > piece[0]]
            aligned = [((a + 0x1fffff) & ~0x1fffff, z & ~0x1fffff) for a, z in free]
            valid = [(a, z - a) for a, z in aligned if z - a >= 0x10200000]
            expected = max(valid, key=lambda r: r[1], default=(0, 0))
            self.assertEqual(self.choose([(0x80000000, 0x380000000)], blocked), expected)


if __name__ == "__main__":
    unittest.main()
