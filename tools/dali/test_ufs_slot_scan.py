"""Execute the production userdata slot scanner and grant rebinder on host fixtures."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsSlotScanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler required")
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base / "Library").mkdir()
        (base / "Uefi.h").write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef bool BOOLEAN; typedef char CHAR8; typedef uint64_t EFI_STATUS;
#define VOID void
#define CONST const
#define STATIC static
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0ULL
#define EFI_INVALID_PARAMETER 2ULL
#define EFI_UNSUPPORTED 3ULL
#define EFI_BAD_BUFFER_SIZE 4ULL
#define EFI_ACCESS_DENIED 15ULL
#define EFI_DEVICE_ERROR 7ULL
#define EFI_OUT_OF_RESOURCES 9ULL
#define EFI_ALREADY_STARTED 20ULL
#define EFI_SECURITY_VIOLATION 26ULL
#define EFI_PAGE_SIZE 4096ULL
#define EFI_SIZE_TO_PAGES(n) (((n) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_UINT64 UINT64_MAX
''')
        (base / "Library/BaseMemoryLib.h").write_text('''#include <string.h>
#define ZeroMem(p,n) memset((p), 0, (n))
#define CopyMem(d,s,n) memcpy((d), (s), (n))
#define CompareMem(a,b,n) memcmp((a), (b), (n))
''')
        (base / "Library/MemoryAllocationLib.h").write_text('''#include <Uefi.h>
VOID *AllocateAlignedPages(UINTN Pages, UINTN Alignment);
VOID FreeAlignedPages(VOID *Buffer, UINTN Pages);
''')
        (base / "Library/BaseCryptLib.h").write_text('#define SHA256_DIGEST_SIZE 32U\n')
        (base / "Library/Mt6991UfsPolicy.h").write_text('''#include <Uefi.h>
#define MT6991_UFS_TARGET_BOOT_B 0U
#define MT6991_UFS_TARGET_EXPDB 1U
#define MT6991_UFS_TARGET_USERDATA_LOGS 2U
#define MT6991_UFS_DATA_LOG_SLOT_BYTES (4ULL * 1024ULL * 1024ULL)
#define MT6991_UFS_DMA_MAX_TRANSFER (1024U * 1024U)
typedef struct {
  UINT64 Blocks; UINT32 BlockSize; BOOLEAN Valid, WriteProtectionKnown, WriteProtected;
} MT6991_UFS_GEOMETRY;
typedef struct {
  BOOLEAN Enabled; UINT8 Lun; CHAR8 Token[33];
  UINT8 DeviceBinding[32], CandidateSha256[32], SourceSha256[32], PartitionGuid[16];
  UINT32 BlockSize;
  UINT64 CapacityBlocks, BootStartLba, BootBlocks, CandidateBytes;
  UINT64 ScratchStartLba, ScratchBlocks, TailGuardBlocks, Sequence;
  UINT32 Target;
} MT6991_UFS_SCRATCH_GRANT;
BOOLEAN Mt6991UfsScratchGrantValid(CONST MT6991_UFS_SCRATCH_GRANT *Grant);
EFI_STATUS Mt6991UfsScratchMapRange(CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST MT6991_UFS_SCRATCH_GRANT *Grant, UINT8 Lun, UINT64 RelativeLba,
  UINTN BufferBytes, BOOLEAN Write, UINT64 *AbsoluteLba, UINT64 *BlockCount);
''')
        (base / "harness.c").write_text(r'''#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#include <UfsSlotScan.h>
#include <UfsGrant.h>
#include <Library/MemoryAllocationLib.h>

#define LOGS_START 64565240ULL
#define LOGS_BLOCKS 8192ULL
#define BLOCK_BYTES 4096U
#define SLOT_BYTES (4U * 1024U * 1024U)
#define PARTITION_BYTES (LOGS_BLOCKS * BLOCK_BYTES)

static UINT8 disk[PARTITION_BYTES];
static UINT64 reads, bad_reads, bytes_read, fail_at;
static UINTN live_allocations;
static UINT64 map_calls, map_absolute, map_count;

VOID *AllocateAlignedPages(UINTN Pages, UINTN Alignment) {
  VOID *Buffer = NULL;
  if (Alignment < sizeof(VOID *)) Alignment = sizeof(VOID *);
  if (posix_memalign(&Buffer, Alignment, Pages * EFI_PAGE_SIZE) != 0) return NULL;
  ++live_allocations;
  return Buffer;
}

VOID FreeAlignedPages(VOID *Buffer, UINTN Pages) {
  (void)Pages;
  if (Buffer != NULL) { free(Buffer); --live_allocations; }
}

static EFI_STATUS ReadFixture(VOID *Context, UINT8 Lun, UINT64 Lba, UINTN Bytes, VOID *Buffer) {
  (void)Context;
  if (Lun != 2 || Bytes == 0 || Bytes > MT6991_UFS_DMA_MAX_TRANSFER ||
      Bytes % BLOCK_BYTES != 0 || Lba < LOGS_START || Lba > LOGS_START + LOGS_BLOCKS ||
      Bytes / BLOCK_BYTES > LOGS_START + LOGS_BLOCKS - Lba ||
      ((uintptr_t)Buffer & (BLOCK_BYTES - 1)) != 0) {
    ++bad_reads; return EFI_SECURITY_VIOLATION;
  }
  if (fail_at != 0 && reads + 1 == fail_at) { ++reads; return EFI_DEVICE_ERROR; }
  memcpy(Buffer, disk + (Lba - LOGS_START) * BLOCK_BYTES, Bytes);
  ++reads; bytes_read += Bytes;
  return EFI_SUCCESS;
}

static DALI_UFS_LAYOUT MakeLayout(UINT64 Preferred) {
  DALI_UFS_LAYOUT L;
  memset(&L, 0, sizeof L);
  L.Verified = TRUE; L.Lun = 2; L.BlockSize = BLOCK_BYTES;
  L.CapacityBlocks = 64577536ULL; L.BootStartLba = LOGS_START; L.BootBlocks = LOGS_BLOCKS;
  L.ScratchBlocks = SLOT_BYTES / BLOCK_BYTES; L.Sequence = Preferred;
  L.Target = MT6991_UFS_TARGET_USERDATA_LOGS;
  const UINT8 Guid[16] = {0xcd,0xe9,0xab,0x32,0x18,0xd8,0x47,0x77,0x8c,0xed,0x69,0x1e,0x7c,0xd5,0xc6,0x36};
  memcpy(L.PartitionGuid, Guid, sizeof Guid);
  L.ScratchStartLba = LOGS_START + (Preferred - 1) * 2 * L.ScratchBlocks;
  L.TailGuardBlocks = LOGS_BLOCKS - (Preferred - 1) * 2 * L.ScratchBlocks - L.ScratchBlocks;
  return L;
}

int RunScan(unsigned Scenario, UINT64 Preferred, UINT32 ParentAlign, UINT32 ScsiAlign, UINT64 Out[10]) {
  memset(disk, 0, sizeof disk);
  reads = bad_reads = bytes_read = fail_at = 0;
  live_allocations = 0;
  if (Scenario == 1) {
    disk[((Preferred - 1) * 2 + 1) * SLOT_BYTES + 19] = 0xA5;
  } else if (Scenario == 2) {
    for (UINTN Slot = 0; Slot < 8; ++Slot) disk[Slot * SLOT_BYTES] = (UINT8)(Slot + 1);
  } else if (Scenario == 3) {
    disk[((Preferred - 1) * 2) * SLOT_BYTES + SLOT_BYTES - 1] = 0x5A;
  } else if (Scenario == 4) {
    fail_at = 5;
  }
  DALI_UFS_LAYOUT L = MakeLayout(Preferred);
  if (Scenario == 6) L.PartitionGuid[0] ^= 1;
  EFI_STATUS Status = DaliUfsSelectEmptyUserdataPair(&L, Preferred,
    ParentAlign, ScsiAlign, ReadFixture, NULL);
  Out[0] = Status; Out[1] = L.Sequence; Out[2] = L.ScratchStartLba;
  Out[3] = L.TailGuardBlocks; Out[4] = reads; Out[5] = bad_reads;
  Out[6] = live_allocations; Out[7] = bytes_read;
  return 0;
}

static MT6991_UFS_SCRATCH_GRANT MakeGrant(UINT64 Sequence) {
  MT6991_UFS_SCRATCH_GRANT G;
  memset(&G, 0, sizeof G);
  G.Enabled = TRUE; G.Target = MT6991_UFS_TARGET_USERDATA_LOGS; G.Lun = 2;
  G.BlockSize = BLOCK_BYTES; G.CapacityBlocks = 64577536ULL;
  G.BootStartLba = LOGS_START; G.BootBlocks = LOGS_BLOCKS;
  G.ScratchBlocks = SLOT_BYTES / BLOCK_BYTES; G.Sequence = Sequence;
  G.ScratchStartLba = LOGS_START + (Sequence - 1) * 2 * G.ScratchBlocks;
  G.TailGuardBlocks = LOGS_BLOCKS - (Sequence - 1) * 2 * G.ScratchBlocks - G.ScratchBlocks;
  for (UINTN I = 0; I < sizeof G.DeviceBinding; ++I) G.DeviceBinding[I] = 0x31;
  for (UINTN I = 0; I < sizeof G.CandidateSha256; ++I) G.CandidateSha256[I] = 0x42;
  for (UINTN I = 0; I < sizeof G.SourceSha256; ++I) G.SourceSha256[I] = 0x53;
  const UINT8 Guid[16] = {0xcd,0xe9,0xab,0x32,0x18,0xd8,0x47,0x77,0x8c,0xed,0x69,0x1e,0x7c,0xd5,0xc6,0x36};
  memcpy(G.PartitionGuid, Guid, sizeof Guid);
  return G;
}

BOOLEAN Mt6991UfsScratchGrantValid(CONST MT6991_UFS_SCRATCH_GRANT *G) {
  if (G == NULL || !G->Enabled || G->Target != MT6991_UFS_TARGET_USERDATA_LOGS ||
      G->Lun != 2 || G->BlockSize != BLOCK_BYTES || G->CapacityBlocks != 64577536ULL ||
      G->BootStartLba != LOGS_START || G->BootBlocks != LOGS_BLOCKS ||
      G->ScratchBlocks != SLOT_BYTES / BLOCK_BYTES || G->Sequence == 0 || G->Sequence > 4) return FALSE;
  UINT64 Offset = (G->Sequence - 1) * 2 * G->ScratchBlocks;
  return G->ScratchStartLba == G->BootStartLba + Offset &&
    G->TailGuardBlocks == G->BootBlocks - Offset - G->ScratchBlocks;
}

EFI_STATUS Mt6991UfsScratchMapRange(CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST MT6991_UFS_SCRATCH_GRANT *G, UINT8 Lun, UINT64 RelativeLba,
  UINTN BufferBytes, BOOLEAN Write, UINT64 *AbsoluteLba, UINT64 *BlockCount) {
  (void)Geometry;
  ++map_calls;
  if (!Mt6991UfsScratchGrantValid(G) || Lun != G->Lun || RelativeLba != 0 ||
      BufferBytes != G->BlockSize || !Write) return EFI_SECURITY_VIOLATION;
  *AbsoluteLba = G->ScratchStartLba; *BlockCount = 1;
  map_absolute = *AbsoluteLba; map_count = *BlockCount;
  return EFI_SUCCESS;
}

int RunAuthorize(unsigned Scenario, UINT64 Out[12]) {
  map_calls = map_absolute = map_count = 0;
  DALI_UFS_GRANT_STATE State;
  memset(&State, 0, sizeof State);
  State.Attempted = State.Prepared = TRUE;
  State.Proposal = MakeGrant(1); State.Sequence = 1;
  DALI_UFS_PROVENANCE Provenance = {0};
  Provenance.Ready = TRUE;
  memcpy(Provenance.CandidateSha256, State.Proposal.CandidateSha256, 32);
  memcpy(Provenance.SourceSha256, State.Proposal.SourceSha256, 32);
  State.Provenance = Provenance;
  DALI_UFS_IDENTITY Identity = {0};
  Identity.Attempted = Identity.Ready = TRUE;
  memcpy(Identity.Digest, State.Proposal.DeviceBinding, 32);
  DALI_UFS_LAYOUT Layout = MakeLayout(3);
  if (Scenario == 1) Identity.Digest[0] ^= 1;
  if (Scenario == 2) Layout.Sequence = 0;
  if (Scenario == 3) Layout.ScratchStartLba++;
  MT6991_UFS_GEOMETRY Geometry[8] = {0};
  MT6991_UFS_SCRATCH_GRANT Live;
  EFI_STATUS Status = DaliUfsGrantAuthorize(&State, &Identity, &Provenance,
    Geometry, &Layout, FALSE, &Live);
  Out[0] = Status; Out[1] = Live.Enabled; Out[2] = Live.Sequence;
  Out[3] = Live.ScratchStartLba; Out[4] = Live.TailGuardBlocks;
  Out[5] = State.Sequence; Out[6] = State.Proposal.Sequence;
  Out[7] = map_calls; Out[8] = map_absolute; Out[9] = map_count;
  return 0;
}
''')
        root = Path(__file__).resolve().parents[2]
        driver = root / "Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe"
        subprocess.run([compiler, "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(base), "-I", str(driver), str(driver / "UfsSlotScan.c"),
                        str(driver / "UfsGrant.c"), str(base / "harness.c"), "-o", str(base / "slot_scan.so")],
                       check=True)
        cls.lib = ctypes.CDLL(str(base / "slot_scan.so"))
        cls.lib.RunScan.argtypes = [ctypes.c_uint, ctypes.c_uint64, ctypes.c_uint32,
                                    ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.RunAuthorize.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scan(self, scenario=0, preferred=1, parent_align=64, scsi_align=64):
        out = (ctypes.c_uint64 * 10)()
        self.lib.RunScan(scenario, preferred, parent_align, scsi_align, out)
        return list(out)

    def authorize(self, scenario=0):
        out = (ctypes.c_uint64 * 12)()
        self.lib.RunAuthorize(scenario, out)
        return list(out)

    def test_scans_exact_partition_in_bounded_aligned_chunks_and_selects_preferred_pair(self):
        out = self.scan(preferred=4)
        self.assertEqual(out[:7], [0, 4, 64565240 + 6 * 1024, 1024, 32, 0, 0])
        self.assertEqual(out[7], 32 * 1024 * 1024)

    def test_nonzero_xnu_sibling_reserves_pair_and_selector_chooses_next_empty_pair(self):
        out = self.scan(scenario=1, preferred=2)
        self.assertEqual(out[0:4], [0, 3, 64565240 + 4 * 1024, 3072])
        self.assertEqual((out[5], out[6]), (0, 0))

    def test_nonzero_data_at_slot_end_is_occupied_and_scanner_wraps(self):
        out = self.scan(scenario=3, preferred=4)
        self.assertEqual(out[1], 1)
        self.assertEqual((out[5], out[6]), (0, 0))

    def test_all_nonzero_pairs_fail_closed_without_reusing_any_slot(self):
        out = self.scan(scenario=2, preferred=1)
        self.assertEqual((out[0], out[4], out[5], out[6]), (9, 32, 0, 0))

    def test_read_failure_propagates_and_allocation_is_released(self):
        out = self.scan(scenario=4)
        self.assertEqual((out[0], out[4], out[5], out[6]), (7, 5, 0, 0))

    def test_wrong_partition_identity_and_unsupported_alignment_fail_before_reads(self):
        wrong_guid = self.scan(scenario=6)
        bad_align = self.scan(parent_align=8192)
        self.assertEqual((wrong_guid[0], wrong_guid[4], wrong_guid[6]), (26, 0, 0))
        self.assertEqual((bad_align[0], bad_align[4], bad_align[6]), (3, 0, 0))

    def test_dynamic_authorizer_rebinds_sequence_window_and_tail_guard(self):
        out = self.authorize()
        self.assertEqual(out[:10], [0, 1, 3, 64565240 + 4 * 1024, 3072,
                                    3, 3, 1, 64565240 + 4 * 1024, 1])

    def test_dynamic_authorizer_rejects_identity_and_layout_mismatches(self):
        for scenario in (1, 2, 3):
            out = self.authorize(scenario)
            self.assertEqual((out[0], out[1], out[7]), (26, 0, 0))


if __name__ == "__main__":
    unittest.main()
