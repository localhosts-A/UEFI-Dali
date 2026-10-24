"""Compile the production policy parser and console writer, without MMIO."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ResetPolicyTests(unittest.TestCase):
    def test_actual_policy_rejects_competing_reset_and_malformed_inputs(self):
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        source = r'''
#include <cassert>
#include <cstring>
#include <string>
#include "DaliBootPolicy.h"
int main() {
  DALI_BOOT_POLICY p = {};
  std::string legacy = "version = 1\nauto_fastboot = true\nwatchdog_timeout_seconds = 31\nrun_token = none\n";
  assert(daliParseBootPolicy(legacy.data(), legacy.size(), &p) && p.AutoFastboot && !p.Qualification);
  const std::string prefix = "version = 2\nauto_fastboot = false\nwatchdog_timeout_seconds = 31\nrun_token = 0123456789abcdef0123456789abcdef\n";
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto input = prefix + "reset_method = " + daliPolicyMethodName(mode) + "\nreset_delay_seconds = 5\n";
    assert(daliParseBootPolicy(input.data(), input.size(), &p));
    assert(p.Qualification && p.Method == mode && !p.AutoFastboot);
    for (unsigned size = 0; size < input.size(); ++size) {
      assert(!daliParseBootPolicy(input.data(), size, &p));
    }
    for (auto extra : {"\n", "reset_method = aee\n", "# ignored\n"}) {
      auto bad = input + extra;
      assert(!daliParseBootPolicy(bad.data(), bad.size(), &p));
    }
  }
  auto good = prefix + "reset_method = psci-cold\nreset_delay_seconds = 5\n";
  for (auto change : {std::pair<std::string,std::string>("false", "true"), {"seconds = 5", "seconds = 1"},
                      {"psci-cold", "psci-cold-extra"}, {"31", "32"},
                      {"0123456789abcdef0123456789abcdef", "none"}}) {
    auto bad = good;
    bad.replace(bad.find(change.first), change.first.size(), change.second);
    assert(!daliParseBootPolicy(bad.data(), bad.size(), &p));
  }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            (work / 'policy.cpp').write_text(source)
            subprocess.run([compiler, '-std=c++14', '-Wall', '-Wextra', '-Werror',
                            '-I', ROOT / 'Platforms/Xiaomi/daliPkg/Drivers/DiagnosticBdsDxe',
                            work / 'policy.cpp', '-o', work / 'policy'], check=True)
            subprocess.run([work / 'policy'], check=True)

    def test_console_retains_valid_headers_and_wraps_without_out_of_bounds(self):
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            (work / 'Base.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t UINT32; typedef uint8_t UINT8; typedef size_t UINTN;
#define STATIC static
#define CONST const
#define VOID void
static inline void MemoryFence(void) { __sync_synchronize(); }
''')
            (work / 'ring.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include "DaliConsole.h"
int main() {
  alignas(4) unsigned char area[12 + 64 + 16];
  memset(area, 0x5a, sizeof area);
  auto ring = reinterpret_cast<DALI_RAM_CONSOLE *>(area);
  ring->Signature = 0x43474244; ring->Start = 3; ring->Size = 3;
  memcpy(area + 12, "old", 3);
  DaliConsoleInitialize(ring, 64);
  assert(ring->Start == 3 && ring->Size == 3 && memcmp(area+12,"old",3) == 0);
  DaliConsoleAppend(ring, 64, reinterpret_cast<const UINT8 *>("new"), 3);
  assert(ring->Start == 6 && ring->Size == 6 && memcmp(area+12,"oldnew",6) == 0);
  ring->Start = 62; ring->Size = 64;
  DaliConsoleAppend(ring, 64, reinterpret_cast<const UINT8 *>("abcd"), 4);
  assert(ring->Start == 2 && ring->Size == 64);
  assert(memcmp(area+12+62,"ab",2) == 0 && memcmp(area+12,"cd",2) == 0);
  for (unsigned n = 0; n < 3; ++n) {
    ring->Signature = n == 0 ? 0 : 0x43474244;
    ring->Start = n == 1 ? 64 : 3; ring->Size = n == 2 ? 65 : 3;
    DaliConsoleInitialize(ring, 64);
    assert(ring->Signature == 0x43474244 && ring->Start == 0 && ring->Size == 0);
  }
  for (unsigned i=76; i<sizeof area; ++i) { assert(area[i] == 0x5a); }
}
''')
            subprocess.run([compiler, '-std=c++14', '-Wall', '-Wextra', '-Werror',
                            '-I', work, '-I', ROOT / 'Silicon/MediaTek/MT6991Pkg/Include/Library',
                            work / 'ring.cpp', '-o', work / 'ring'], check=True)
            subprocess.run([work / 'ring'], check=True)


if __name__ == '__main__':
    unittest.main()
