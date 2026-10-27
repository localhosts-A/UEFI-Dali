"""Exercise the production v1/v2 userdata log classifier on synthetic RAM media.

The test SHA implementation is deterministic but intentionally non-cryptographic;
these cases cover scanner behavior and wire consistency, not SHA-256 security.
"""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsLogSlotScanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler required")
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base / "Library").mkdir()
        (base / "Library/Mt6991UfsPolicy.h").write_text(r'''#pragma once
#include <Uefi.h>
#define MT6991_UFS_TARGET_USERDATA_LOGS 2U
#define MT6991_UFS_DATA_LOG_SLOT_BYTES (4ULL * 1024ULL * 1024ULL)
#define MT6991_UFS_DMA_MAX_TRANSFER (1024U * 1024U)
typedef struct {
  BOOLEAN Enabled; UINT32 Target; UINT8 Lun; UINT32 BlockSize;
  UINT64 CapacityBlocks, BootStartLba, BootBlocks, ScratchBlocks;
  UINT64 ScratchStartLba, TailGuardBlocks, Sequence;
  UINT8 PartitionGuid[16], DeviceBinding[32], CandidateSha256[32], SourceSha256[32];
  UINT8 Token[32];
} MT6991_UFS_SCRATCH_GRANT;
typedef struct { UINT64 Blocks; UINT32 BlockSize; BOOLEAN Valid, WriteProtectionKnown, WriteProtected; } MT6991_UFS_GEOMETRY;
BOOLEAN Mt6991UfsScratchGrantValid(CONST MT6991_UFS_SCRATCH_GRANT *Grant);
''')
        (base / "Uefi.h").write_text(r'''#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef uint8_t BOOLEAN; typedef uint64_t EFI_STATUS;
#define VOID void
#define CONST const
#define STATIC static
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0ULL
#define EFI_INVALID_PARAMETER (0x8000000000000000ULL | 2ULL)
#define EFI_UNSUPPORTED (0x8000000000000000ULL | 3ULL)
#define EFI_BAD_BUFFER_SIZE (0x8000000000000000ULL | 4ULL)
#define EFI_DEVICE_ERROR (0x8000000000000000ULL | 7ULL)
#define EFI_OUT_OF_RESOURCES (0x8000000000000000ULL | 9ULL)
#define EFI_SECURITY_VIOLATION (0x8000000000000000ULL | 26ULL)
#define EFI_COMPROMISED_DATA (0x8000000000000000ULL | 33ULL)
#define EFI_PAGE_SIZE 4096ULL
#define EFI_SIZE_TO_PAGES(n) (((n) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)
#define MAX_UINT64 UINT64_MAX
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
''')
        (base / "Library/BaseMemoryLib.h").write_text(r'''#pragma once
#include <string.h>
#define ZeroMem(p,n) memset((p), 0, (n))
#define CopyMem(d,s,n) memcpy((d), (s), (n))
#define CompareMem(a,b,n) memcmp((a), (b), (n))
''')
        (base / "Library/MemoryAllocationLib.h").write_text(r'''#pragma once
#include <Uefi.h>
VOID *AllocateAlignedPages(UINTN Pages, UINTN Alignment);
VOID FreeAlignedPages(VOID *Buffer, UINTN Pages);
VOID *AllocatePool(UINTN Bytes);
VOID FreePool(VOID *Buffer);
''')
        (base / "Library/BaseCryptLib.h").write_text(r'''#pragma once
#include <Uefi.h>
#define SHA256_DIGEST_SIZE 32U
UINTN Sha256GetContextSize(VOID);
BOOLEAN Sha256Init(VOID *Context);
BOOLEAN Sha256Update(VOID *Context, CONST VOID *Data, UINTN DataSize);
BOOLEAN Sha256Final(VOID *Context, UINT8 HashValue[SHA256_DIGEST_SIZE]);
BOOLEAN Sha256HashAll(CONST VOID *Data, UINTN DataSize, UINT8 HashValue[SHA256_DIGEST_SIZE]);
''')
        (base / "harness.c").write_text(r'''#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#include <UfsLogSlotScan.h>
#include <Library/BaseCryptLib.h>
#include <Library/MemoryAllocationLib.h>

#define LOGS_START 64565240ULL
#define LOGS_BLOCKS 8192ULL
#define BLOCK_BYTES 4096U
#define SLOT_BYTES (4U * 1024U * 1024U)
#define SLOT_BLOCKS (SLOT_BYTES / BLOCK_BYTES)
#define PARTITION_BYTES (LOGS_BLOCKS * BLOCK_BYTES)
#define BODY_OFFSET (2U * BLOCK_BYTES)
#define BODY_LENGTH 8U
#define HASH_AT 212U

typedef struct { UINT64 State, Bytes; } MOCK_SHA256;
static UINT8 disk[PARTITION_BYTES];
static UINT64 reads, bad_reads, fail_at;
static UINTN live_allocations;

UINTN Sha256GetContextSize(VOID) { return sizeof(MOCK_SHA256); }
BOOLEAN Sha256Init(VOID *Context) {
  MOCK_SHA256 *Hash = Context;
  if (Hash == NULL) return FALSE;
  Hash->State = 0xcbf29ce484222325ULL; Hash->Bytes = 0; return TRUE;
}
BOOLEAN Sha256Update(VOID *Context, CONST VOID *Data, UINTN DataSize) {
  MOCK_SHA256 *Hash = Context; CONST UINT8 *Bytes = Data;
  if (Hash == NULL || (DataSize != 0 && Data == NULL)) return FALSE;
  for (UINTN I = 0; I < DataSize; ++I) {
    Hash->State = (Hash->State ^ Bytes[I]) * 0x100000001b3ULL;
    Hash->State ^= Hash->State >> 29; ++Hash->Bytes;
  }
  return TRUE;
}
BOOLEAN Sha256Final(VOID *Context, UINT8 Digest[SHA256_DIGEST_SIZE]) {
  MOCK_SHA256 *Hash = Context; UINT64 State;
  if (Hash == NULL || Digest == NULL) return FALSE;
  State = Hash->State ^ (Hash->Bytes * 0x9e3779b185ebca87ULL);
  for (UINTN I = 0; I < SHA256_DIGEST_SIZE; ++I) {
    State ^= State >> 12; State *= 0x9e3779b185ebca87ULL;
    Digest[I] = (UINT8)(State >> ((I & 7U) * 8U));
  }
  return TRUE;
}
BOOLEAN Sha256HashAll(CONST VOID *Data, UINTN DataSize, UINT8 Digest[SHA256_DIGEST_SIZE]) {
  MOCK_SHA256 Hash;
  return Sha256Init(&Hash) && Sha256Update(&Hash, Data, DataSize) && Sha256Final(&Hash, Digest);
}

VOID *AllocateAlignedPages(UINTN Pages, UINTN Alignment) {
  VOID *Buffer = NULL;
  if (Alignment < sizeof(VOID *)) Alignment = sizeof(VOID *);
  if (posix_memalign(&Buffer, Alignment, Pages * EFI_PAGE_SIZE) != 0) return NULL;
  ++live_allocations; return Buffer;
}
VOID FreeAlignedPages(VOID *Buffer, UINTN Pages) {
  (void)Pages;
  if (Buffer != NULL) { free(Buffer); --live_allocations; }
}
VOID *AllocatePool(UINTN Bytes) {
  VOID *Buffer = malloc(Bytes);
  if (Buffer != NULL) ++live_allocations;
  return Buffer;
}
VOID FreePool(VOID *Buffer) {
  if (Buffer != NULL) { free(Buffer); --live_allocations; }
}

static VOID Put32(UINT8 *P, UINT32 Value) {
  for (UINTN I = 0; I < 4; ++I) P[I] = (UINT8)(Value >> (8U * I));
}
static VOID Put64(UINT8 *P, UINT64 Value) {
  for (UINTN I = 0; I < 8; ++I) P[I] = (UINT8)(Value >> (8U * I));
}
static CONST UINT8 Guid[16] = {0xcd,0xe9,0xab,0x32,0x18,0xd8,0x47,0x77,0x8c,0xed,0x69,0x1e,0x7c,0xd5,0xc6,0x36};

BOOLEAN Mt6991UfsScratchGrantValid(CONST MT6991_UFS_SCRATCH_GRANT *G) {
  return G != NULL && G->Enabled && G->Target == MT6991_UFS_TARGET_USERDATA_LOGS &&
    G->Lun == 2 && G->BlockSize == BLOCK_BYTES && G->CapacityBlocks == 64577536ULL &&
    G->BootStartLba == LOGS_START && G->BootBlocks == LOGS_BLOCKS &&
    G->ScratchBlocks == SLOT_BLOCKS && G->Sequence > 0 && G->Sequence <= 4;
}

static EFI_STATUS ReadFixture(VOID *Context, UINT8 Lun, UINT64 Lba, UINTN Bytes, VOID *Buffer) {
  (void)Context;
  if (Lun != 2 || Bytes == 0 || Bytes > MT6991_UFS_DMA_MAX_TRANSFER ||
      Bytes % BLOCK_BYTES != 0 || Lba < LOGS_START || Lba > LOGS_START + LOGS_BLOCKS ||
      Bytes / BLOCK_BYTES > LOGS_START + LOGS_BLOCKS - Lba ||
      ((uintptr_t)Buffer & (EFI_PAGE_SIZE - 1)) != 0) {
    ++bad_reads; return EFI_SECURITY_VIOLATION;
  }
  ++reads;
  if (fail_at != 0 && reads == fail_at) return EFI_DEVICE_ERROR;
  memcpy(Buffer, disk + (Lba - LOGS_START) * BLOCK_BYTES, Bytes);
  return EFI_SUCCESS;
}

static DALI_UFS_LAYOUT MakeLayout(VOID) {
  DALI_UFS_LAYOUT L; memset(&L, 0, sizeof L);
  L.Verified = TRUE; L.Lun = 2; L.BlockSize = BLOCK_BYTES;
  L.CapacityBlocks = 64577536ULL; L.BootStartLba = LOGS_START;
  L.BootBlocks = LOGS_BLOCKS; L.ScratchBlocks = SLOT_BLOCKS;
  L.Target = MT6991_UFS_TARGET_USERDATA_LOGS; memcpy(L.PartitionGuid, Guid, sizeof Guid);
  return L;
}
static MT6991_UFS_SCRATCH_GRANT MakeGrant(VOID) {
  MT6991_UFS_SCRATCH_GRANT G; memset(&G, 0, sizeof G);
  G.Enabled = TRUE; G.Target = MT6991_UFS_TARGET_USERDATA_LOGS; G.Lun = 2;
  G.BlockSize = BLOCK_BYTES; G.CapacityBlocks = 64577536ULL;
  G.BootStartLba = LOGS_START; G.BootBlocks = LOGS_BLOCKS;
  G.ScratchBlocks = SLOT_BLOCKS; G.Sequence = 1;
  memcpy(G.PartitionGuid, Guid, sizeof Guid);
  memset(G.DeviceBinding, 0x31, sizeof G.DeviceBinding);
  memset(G.CandidateSha256, 0x42, sizeof G.CandidateSha256);
  memset(G.SourceSha256, 0x53, sizeof G.SourceSha256);
  return G;
}

static VOID BuildRecord(UINT8 *Record, UINT32 Kind, UINT32 Schema, UINT32 Owner,
  UINT64 Sequence, UINT64 Generation, CONST UINT8 *Token,
  CONST UINT8 *Body, UINT32 BodyBytes, BOOLEAN ZeroCandidate) {
  STATIC CONST UINT8 MagicV1[16] = "DALIUFSLOG1";
  STATIC CONST UINT8 MagicV2[16] = "DALIUFSLOG2";
  UINT8 Digest[SHA256_DIGEST_SIZE];
  memset(Record, 0, BLOCK_BYTES);
  memcpy(Record, Schema == 1 ? MagicV1 : MagicV2, 16);
  Put32(Record + 16, Kind); Put32(Record + 20, Schema);
  memcpy(Record + 24, Token, 32); Put32(Record + 56, Owner);
  Put64(Record + 60, Sequence); Put32(Record + 68, BodyBytes);
  Put32(Record + 72, BLOCK_BYTES); Put64(Record + 76, SLOT_BLOCKS);
  memset(Record + 84, 0x31, 32);
  if (!ZeroCandidate) memset(Record + 116, 0x42, 32);
  memset(Record + 148, 0x53, 32);
  Sha256HashAll(Body, BodyBytes, Digest); memcpy(Record + 180, Digest, sizeof Digest);
  if (Schema == 2) {
    Put64(Record + 244, Generation); Put32(Record + 252, 0);
    Put32(Record + 256, Owner == 1 ? 1 : 2); Put32(Record + 260, Owner == 1 ? 1 : 2);
  }
  Sha256HashAll(Record, BLOCK_BYTES, Digest); memcpy(Record + HASH_AT, Digest, sizeof Digest);
}

static VOID WriteSlot(UINTN Slot, UINT32 Schema, UINT64 Generation,
  CONST UINT8 *Token, BOOLEAN AttemptOnly, BOOLEAN ZeroCandidate, BOOLEAN ValidXnuBody) {
  UINT32 Owner = (Slot & 1U) == 0 ? 1U : 2U;
  UINT64 Sequence = Slot / 2U + 1U;
  UINT32 BodyBytes = Owner == 2 && Schema == 2 ? 66U : BODY_LENGTH;
  UINT8 *Base = disk + Slot * SLOT_BYTES;
  UINT8 Body[66] = {0};
  if (Owner == 2 && Schema == 2) {
    if (ValidXnuBody) {
      memcpy(Body, "DALI-XNU-BODY1", 14); Put32(Body + 16, 1); Put32(Body + 20, 64);
      Put32(Body + 24, 1); Put32(Body + 28, 1); Put32(Body + 32, 1);
      Put32(Body + 48, 2U * 1024U * 1024U - 12U); Put32(Body + 60, 66);
      Body[64] = 'd'; Body[65] = 'm';
    } else {
      memset(Body, 0xA5, BodyBytes);
    }
  } else {
    memcpy(Body, "log-body", BODY_LENGTH);
  }
  memcpy(Base + BODY_OFFSET, Body, BodyBytes);
  BuildRecord(Base, 1, Schema, Owner, Sequence, Generation, Token, Body, BodyBytes, ZeroCandidate);
  if (AttemptOnly) return;
  BuildRecord(Base + BLOCK_BYTES, 2, Schema, Owner, Sequence, Generation, Token, Body, BodyBytes, ZeroCandidate);
  BuildRecord(Base + SLOT_BYTES - BLOCK_BYTES, 3, Schema, Owner, Sequence, Generation, Token, Body, BodyBytes, ZeroCandidate);
}

int RunScenario(unsigned Scenario, UINT64 Out[27]) {
  static CONST UINT8 TokenA[] = "11111111111111111111111111111111";
  static CONST UINT8 TokenB[] = "22222222222222222222222222222222";
  memset(disk, 0, sizeof disk); reads = bad_reads = fail_at = live_allocations = 0;
  DALI_UFS_LAYOUT Layout = MakeLayout(); MT6991_UFS_SCRATCH_GRANT Grant = MakeGrant();
  if (Scenario == 1) {
    WriteSlot(0, 1, 0, TokenA, FALSE, FALSE, TRUE);
    WriteSlot(2, 2, 4, TokenA, FALSE, FALSE, TRUE);
  } else if (Scenario == 2) {
    WriteSlot(0, 2, 17, TokenA, FALSE, FALSE, TRUE);
    WriteSlot(1, 2, 17, TokenA, FALSE, FALSE, TRUE);
  } else if (Scenario == 3) {
    WriteSlot(1, 2, 17, TokenA, FALSE, FALSE, TRUE);
  } else if (Scenario == 4) {
    WriteSlot(0, 2, 5, TokenA, FALSE, FALSE, TRUE);
    WriteSlot(1, 2, 6, TokenB, FALSE, FALSE, TRUE);
  } else if (Scenario == 5) {
    WriteSlot(0, 2, 7, TokenA, FALSE, FALSE, TRUE);
    WriteSlot(2, 2, 7, TokenB, FALSE, FALSE, TRUE);
  } else if (Scenario == 6) {
    WriteSlot(0, 2, 8, TokenA, TRUE, FALSE, TRUE);
  } else if (Scenario == 7) {
    WriteSlot(0, 2, 9, TokenA, FALSE, FALSE, TRUE);
    disk[BODY_OFFSET] ^= 0x01;
  } else if (Scenario == 8) {
    WriteSlot(1, 2, 10, TokenA, FALSE, FALSE, FALSE);
  } else if (Scenario == 9) {
    WriteSlot(0, 2, 11, TokenA, FALSE, TRUE, TRUE);
  } else if (Scenario == 10) {
    WriteSlot(0, 2, UINT64_MAX, TokenA, FALSE, FALSE, TRUE);
  } else if (Scenario == 11) {
    WriteSlot(0, 1, 0, TokenA, FALSE, FALSE, TRUE); fail_at = 5;
  } else if (Scenario == 12) {
    Layout.PartitionGuid[0] ^= 1;
  }
  DALI_UFS_LOG_SLOT_SCAN_RESULT Result;
  EFI_STATUS Status = DaliUfsClassifyUserdataLogSlots(&Layout, &Grant, 64, 64,
    ReadFixture, NULL, &Result);
  Out[0] = Status;
  for (UINTN I = 0; I < 8; ++I) Out[1 + I] = Result.Slots[I];
  Out[9] = Result.HasCommittedV2; Out[10] = Result.MaxGeneration;
  Out[11] = Result.NextGeneration; Out[12] = Result.NextGenerationValid;
  Out[13] = Result.HasInconsistentPair;
  for (UINTN I = 0; I < 4; ++I) Out[14 + I] = Result.PairConsistent[I];
  Out[18] = reads; Out[19] = bad_reads; Out[20] = live_allocations;
  Out[21] = Result.CommittedUefiV2Count;
  Out[22] = Result.UefiV2[1].Valid;
  Out[23] = Result.UefiV2[1].Generation;
  Out[24] = Result.UefiV2[1].Token[0];
  Out[25] = Result.UefiV2[1].CandidateSha256[0];
  Out[26] = Result.UefiV2[1].BodySha256[0];
  return 0;
}
''')
        root = Path(__file__).resolve().parents[2]
        driver = root / "Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe"
        subprocess.run([compiler, "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(base), "-I", str(driver), str(driver / "UfsLogSlotScan.c"),
                        str(base / "harness.c"), "-o", str(base / "log_slot_scan.so")], check=True)
        cls.lib = ctypes.CDLL(str(base / "log_slot_scan.so"))
        cls.lib.RunScenario.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scan(self, scenario=0):
        out = (ctypes.c_uint64 * 27)()
        self.assertEqual(self.lib.RunScenario(scenario, out), 0)
        return list(out)

    def test_empty_partition_starts_generation_one_with_bounded_reads(self):
        out = self.scan()
        self.assertEqual((out[0], out[11], out[12], out[18:21]), (0, 1, 1, [32, 0, 0]))
        self.assertEqual(out[1:9], [0] * 8)

    def test_valid_v1_is_preserved_and_committed_v2_sets_next_generation(self):
        out = self.scan(1)
        self.assertEqual((out[0], out[1], out[3], out[9], out[10], out[11], out[12]),
                         (0, 1, 2, 1, 4, 5, 1))
        self.assertEqual((out[21], out[22], out[23], out[24], out[25]), (1, 1, 4, ord("1"), 0x42))
        self.assertNotEqual(out[26], 0)

    def test_matching_uefi_and_xnu_v2_pair_is_valid(self):
        out = self.scan(2)
        self.assertEqual((out[0], out[1], out[2], out[10], out[11], out[14]),
                         (0, 2, 2, 17, 18, 1))

    def test_orphan_xnu_v2_is_preserved_but_does_not_count_as_committed_generation(self):
        out = self.scan(3)
        self.assertEqual((out[0], out[2], out[9], out[11], out[12], out[13], out[14]),
                         (0, 2, 0, 1, 1, 1, 0))

    def test_inconsistent_v2_pair_does_not_block_a_separate_empty_pair(self):
        out = self.scan(4)
        self.assertEqual((out[0], out[10], out[11], out[12], out[13], out[14]),
                         (0, 5, 6, 1, 1, 0))

    def test_duplicate_v2_generations_fail_closed(self):
        out = self.scan(5)
        self.assertEqual(out[0], 0x8000000000000000 | 33)
        self.assertEqual(out[12], 0)

    def test_v2_attempt_without_a_matching_commit_is_interrupted(self):
        out = self.scan(6)
        self.assertEqual((out[0], out[1], out[9], out[11], out[12]), (0, 3, 0, 1, 1))

    def test_body_corruption_and_invalid_xnu_body_are_not_valid_records(self):
        corrupted = self.scan(7); malformed_xnu = self.scan(8)
        self.assertEqual((corrupted[0], corrupted[1], corrupted[9]), (0, 4, 0))
        self.assertEqual((malformed_xnu[0], malformed_xnu[2], malformed_xnu[9]), (0, 4, 0))

    def test_zero_input_binding_is_not_validated_as_v2(self):
        out = self.scan(9)
        self.assertEqual((out[0], out[1], out[9], out[11]), (0, 4, 0, 1))

    def test_generation_overflow_and_body_read_failure_fail_closed(self):
        overflow = self.scan(10); io_error = self.scan(11)
        self.assertEqual(overflow[0], 0x8000000000000000 | 9)
        self.assertEqual(overflow[12], 0)
        self.assertEqual(io_error[0], 0x8000000000000000 | 7)
        self.assertEqual(io_error[18], 5)

    def test_partition_identity_mismatch_is_rejected_before_allocation_or_io(self):
        out = self.scan(12)
        self.assertEqual((out[0], out[18], out[20]), (0x8000000000000000 | 26, 0, 0))


if __name__ == "__main__":
    unittest.main()
