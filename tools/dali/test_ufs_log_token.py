"""Exercise the production UFS record writer with a synthetic BlockIo disk.

The host SHA shim is deterministic but intentionally not cryptographic. These
cases cover runtime token plumbing and record verification, not SHA-256 itself.
"""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsLogTokenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler required")
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base / "Library").mkdir()
        (base / "Protocol").mkdir()
        (base / "Uefi.h").write_text(r'''#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef bool BOOLEAN; typedef uint64_t EFI_STATUS;
#define VOID void
#define CONST const
#define STATIC static
#define EFIAPI
#define TRUE true
#define FALSE false
#define EFI_SUCCESS 0ULL
#define EFI_ERROR(Status) (((Status) & 0x8000000000000000ULL) != 0)
#define EFI_INVALID_PARAMETER (0x8000000000000000ULL | 2ULL)
#define EFI_UNSUPPORTED (0x8000000000000000ULL | 3ULL)
#define EFI_BAD_BUFFER_SIZE (0x8000000000000000ULL | 4ULL)
#define EFI_DEVICE_ERROR (0x8000000000000000ULL | 7ULL)
#define EFI_OUT_OF_RESOURCES (0x8000000000000000ULL | 9ULL)
#define EFI_ACCESS_DENIED (0x8000000000000000ULL | 15ULL)
#define EFI_ALREADY_STARTED (0x8000000000000000ULL | 20ULL)
#define EFI_SECURITY_VIOLATION (0x8000000000000000ULL | 26ULL)
#define EFI_COMPROMISED_DATA (0x8000000000000000ULL | 33ULL)
#define EFI_PAGE_SIZE 4096ULL
#define MAX_UINTN SIZE_MAX
#define MAX_UINT64 UINT64_MAX
''')
        (base / "Protocol/BlockIo.h").write_text(r'''#pragma once
#include <Uefi.h>
typedef struct {
  UINT32 MediaId;
  BOOLEAN RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
  UINT32 BlockSize, IoAlign;
  UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;
typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(EFI_BLOCK_IO_PROTOCOL *, UINT32, UINT64, UINTN, VOID *);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_WRITE)(EFI_BLOCK_IO_PROTOCOL *, UINT32, UINT64, UINTN, CONST VOID *);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_FLUSH)(EFI_BLOCK_IO_PROTOCOL *);
struct _EFI_BLOCK_IO_PROTOCOL {
  EFI_BLOCK_IO_MEDIA *Media;
  EFI_BLOCK_READ ReadBlocks;
  EFI_BLOCK_WRITE WriteBlocks;
  EFI_BLOCK_FLUSH FlushBlocks;
};
''')
        (base / "Library/BaseMemoryLib.h").write_text(r'''#pragma once
#include <string.h>
#define ZeroMem(p,n) memset((p), 0, (n))
#define CopyMem(d,s,n) memcpy((d), (s), (n))
#define CompareMem(a,b,n) memcmp((a), (b), (n))
''')
        (base / "Library/BaseCryptLib.h").write_text(r'''#pragma once
#include <Uefi.h>
#define SHA256_DIGEST_SIZE 32U
BOOLEAN Sha256HashAll(CONST VOID *Data, UINTN DataSize, UINT8 HashValue[SHA256_DIGEST_SIZE]);
''')
        (base / "Library/Mt6991UfsPolicy.h").write_text(r'''#pragma once
#include <Uefi.h>
#define MT6991_UFS_TARGET_EXPDB 1U
#define MT6991_UFS_TARGET_USERDATA_LOGS 2U
typedef struct {
  BOOLEAN Enabled;
  UINT32 Target;
  UINT8 Token[32];
  UINT8 DeviceBinding[32], CandidateSha256[32], SourceSha256[32];
  UINT32 BlockSize;
  UINT64 ScratchBlocks, Sequence;
} MT6991_UFS_SCRATCH_GRANT;
BOOLEAN Mt6991UfsScratchGrantValid(CONST MT6991_UFS_SCRATCH_GRANT *Grant);
''')
        (base / "harness.c").write_text(r'''#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <UfsLog.h>

#define BLOCK_BYTES 4096U
#define SLOT_BLOCKS 1024U
#define SLOT_BYTES (BLOCK_BYTES * SLOT_BLOCKS)
#define MEDIA_ID 7U
#include <Library/BaseCryptLib.h>

typedef struct {
  UINT64 Status, Attempted, Verified, Reads, Writes, Flushes, GrantUnchanged;
  UINT64 WrongReadStatus, WrongGenerationStatus;
  UINT8 RecordTokens[3][32];
  UINT8 GrantToken[32];
  UINT32 RecordVersions[3];
  UINT64 RecordGenerations[3];
} HOST_RESULT;

static UINT8 disk[SLOT_BYTES];
static UINT64 reads, writes, flushes;
static EFI_BLOCK_IO_MEDIA media;
static EFI_BLOCK_IO_PROTOCOL io;

BOOLEAN Sha256HashAll(CONST VOID *Data, UINTN DataSize, UINT8 HashValue[SHA256_DIGEST_SIZE]) {
  CONST UINT8 *Bytes = Data;
  UINT32 State = 0x811c9dc5U;
  for (UINTN I = 0; I < DataSize; ++I) {
    State = (State ^ Bytes[I]) * 16777619U;
    State = (State << 7) | (State >> 25);
  }
  for (UINTN I = 0; I < SHA256_DIGEST_SIZE; ++I) {
    State ^= State >> 13; State *= 0x5bd1e995U; State ^= State >> 15;
    HashValue[I] = (UINT8)(State >> ((I & 3U) * 8U));
  }
  return TRUE;
}

BOOLEAN Mt6991UfsScratchGrantValid(CONST MT6991_UFS_SCRATCH_GRANT *Grant) {
  return Grant != NULL && Grant->Enabled &&
    Grant->Target == MT6991_UFS_TARGET_USERDATA_LOGS &&
    Grant->BlockSize == BLOCK_BYTES && Grant->ScratchBlocks == SLOT_BLOCKS;
}

static EFI_STATUS ReadBlocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  UINT64 Lba, UINTN BufferSize, VOID *Buffer) {
  if (This != &io || MediaId != MEDIA_ID || Buffer == NULL || BufferSize == 0 ||
      BufferSize % BLOCK_BYTES != 0 || Lba > SLOT_BLOCKS ||
      BufferSize / BLOCK_BYTES > SLOT_BLOCKS - Lba) { return EFI_ACCESS_DENIED; }
  memcpy(Buffer, disk + Lba * BLOCK_BYTES, BufferSize);
  ++reads;
  return EFI_SUCCESS;
}

static EFI_STATUS WriteBlocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  UINT64 Lba, UINTN BufferSize, CONST VOID *Buffer) {
  if (This != &io || MediaId != MEDIA_ID || media.ReadOnly || Buffer == NULL ||
      BufferSize == 0 || BufferSize % BLOCK_BYTES != 0 || Lba > SLOT_BLOCKS ||
      BufferSize / BLOCK_BYTES > SLOT_BLOCKS - Lba) { return EFI_ACCESS_DENIED; }
  memcpy(disk + Lba * BLOCK_BYTES, Buffer, BufferSize);
  ++writes;
  return EFI_SUCCESS;
}

static EFI_STATUS FlushBlocks(EFI_BLOCK_IO_PROTOCOL *This) {
  if (This != &io) { return EFI_INVALID_PARAMETER; }
  ++flushes;
  return EFI_SUCCESS;
}

static void MakeGrant(MT6991_UFS_SCRATCH_GRANT *Grant) {
  static CONST UINT8 BuildToken[] = "0123456789abcdef0123456789abcdef";
  memset(Grant, 0, sizeof(*Grant));
  Grant->Enabled = TRUE;
  Grant->Target = MT6991_UFS_TARGET_USERDATA_LOGS;
  Grant->BlockSize = BLOCK_BYTES;
  Grant->ScratchBlocks = SLOT_BLOCKS;
  Grant->Sequence = 1;
  memcpy(Grant->Token, BuildToken, sizeof Grant->Token);
  memset(Grant->DeviceBinding, 0x31, sizeof Grant->DeviceBinding);
  memset(Grant->CandidateSha256, 0x42, sizeof Grant->CandidateSha256);
  memset(Grant->SourceSha256, 0x53, sizeof Grant->SourceSha256);
}

static void ResetDisk(VOID) {
  memset(disk, 0, sizeof disk);
  reads = writes = flushes = 0;
  memset(&media, 0, sizeof media);
  media.MediaId = MEDIA_ID;
  media.MediaPresent = TRUE;
  media.LogicalPartition = TRUE;
  media.BlockSize = BLOCK_BYTES;
  media.IoAlign = BLOCK_BYTES;
  media.LastBlock = SLOT_BLOCKS - 1;
  io.Media = &media;
  io.ReadBlocks = ReadBlocks;
  io.WriteBlocks = WriteBlocks;
  io.FlushBlocks = FlushBlocks;
}

static EFI_STATUS RunRecord(CONST UINT8 *Token, UINT64 Generation, BOOLEAN ReadbackOnly,
  DALI_UFS_LOG_RESULT *Result, VOID *Work) {
  static CONST UINT8 Body[] = "uefi-console-record";
  MT6991_UFS_SCRATCH_GRANT Grant;
  MakeGrant(&Grant);
  return DaliUfsLogRun(&io, &Grant, DALI_UFS_LOG_OWNER_UEFI, 1, Generation, Token,
    ReadbackOnly, ReadbackOnly ? NULL : Body,
    ReadbackOnly ? 0 : sizeof Body - 1, Work, DALI_UFS_LOG_WORK_BYTES, Result);
}

int RunWriteWithGeneration(CONST UINT8 *Token, UINT64 Generation, HOST_RESULT *Out) {
  if (Out == NULL) { return 1; }
  memset(Out, 0, sizeof *Out);
  ResetDisk();
  MT6991_UFS_SCRATCH_GRANT GrantBefore, GrantAfter;
  MakeGrant(&GrantBefore);
  GrantAfter = GrantBefore;
  VOID *Work = NULL;
  if (posix_memalign(&Work, BLOCK_BYTES, DALI_UFS_LOG_WORK_BYTES) != 0) return 2;
  DALI_UFS_LOG_RESULT Result = {0};
  static CONST UINT8 Body[] = "uefi-console-record";
  Out->Status = DaliUfsLogRun(&io, &GrantAfter, DALI_UFS_LOG_OWNER_UEFI, 1, Generation,
    Token, FALSE, Body, sizeof Body - 1, Work, DALI_UFS_LOG_WORK_BYTES, &Result);
  Out->Attempted = Result.Attempted;
  Out->Verified = Result.Verified;
  Out->Reads = reads; Out->Writes = writes; Out->Flushes = flushes;
  Out->GrantUnchanged = memcmp(&GrantBefore, &GrantAfter, sizeof GrantBefore) == 0;
  memcpy(Out->GrantToken, GrantAfter.Token, sizeof Out->GrantToken);
  for (UINTN I = 0; I < 3; ++I) {
    UINTN At = I == 2 ? SLOT_BYTES - BLOCK_BYTES : I * BLOCK_BYTES;
    memcpy(Out->RecordTokens[I], disk + At + 24, 32);
    Out->RecordVersions[I] = (UINT32)disk[At + 20] | (UINT32)disk[At + 21] << 8 |
      (UINT32)disk[At + 22] << 16 | (UINT32)disk[At + 23] << 24;
    for (UINTN B = 0; B < 8; ++B) Out->RecordGenerations[I] |= (UINT64)disk[At + 244 + B] << (8U * B);
  }
  free(Work);
  return 0;
}

int RunWrite(CONST UINT8 *Token, HOST_RESULT *Out) {
  return RunWriteWithGeneration(Token, 1, Out);
}
int RunWriteV2(CONST UINT8 *Token, UINT64 Generation, HOST_RESULT *Out) {
  return RunWriteWithGeneration(Token, Generation, Out);
}

int RunRoundTripWithGeneration(CONST UINT8 *WriteToken, CONST UINT8 *ReadToken,
  CONST UINT8 *WrongToken, UINT64 Generation, HOST_RESULT *Out) {
  if (Out == NULL) { return 1; }
  memset(Out, 0, sizeof *Out);
  ResetDisk();
  VOID *Work = NULL;
  if (posix_memalign(&Work, BLOCK_BYTES, DALI_UFS_LOG_WORK_BYTES) != 0) return 2;
  DALI_UFS_LOG_RESULT WriteResult = {0}, ReadResult = {0}, WrongResult = {0};
  Out->Status = RunRecord(WriteToken, Generation, FALSE, &WriteResult, Work);
  RunRecord(ReadToken, Generation, TRUE, &ReadResult, Work);
  Out->Verified = ReadResult.Verified;
  Out->Reads = reads; Out->Writes = writes; Out->Flushes = flushes;
  Out->Attempted = ReadResult.Attempted;
  if (WrongToken != NULL) {
    Out->WrongReadStatus = RunRecord(WrongToken, Generation, TRUE, &WrongResult, Work);
    Out->Reads = reads; Out->Writes = writes; Out->Flushes = flushes;
  }
  if (Generation != 0) {
    memset(&WrongResult, 0, sizeof WrongResult);
    Out->WrongGenerationStatus = RunRecord(ReadToken, Generation + 1, TRUE, &WrongResult, Work);
    Out->Reads = reads; Out->Writes = writes; Out->Flushes = flushes;
  }
  free(Work);
  return 0;
}

int RunRoundTrip(CONST UINT8 *WriteToken, CONST UINT8 *ReadToken,
  CONST UINT8 *WrongToken, HOST_RESULT *Out) {
  return RunRoundTripWithGeneration(WriteToken, ReadToken, WrongToken, 1, Out);
}
int RunRoundTripV2(CONST UINT8 *WriteToken, CONST UINT8 *ReadToken,
  CONST UINT8 *WrongToken, UINT64 Generation, HOST_RESULT *Out) {
  return RunRoundTripWithGeneration(WriteToken, ReadToken, WrongToken, Generation, Out);
}
''')
        root = Path(__file__).resolve().parents[2]
        driver = root / "Silicon/MediaTek/MT6991Pkg/Drivers/UfsHostDxe"
        subprocess.run([compiler, "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(base), "-I", str(driver), str(driver / "UfsLog.c"),
                        str(base / "harness.c"), "-o", str(base / "ufs_log.so")], check=True)
        cls.lib = ctypes.CDLL(str(base / "ufs_log.so"))
        cls.lib.RunWrite.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_void_p]
        cls.lib.RunWriteV2.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint64, ctypes.c_void_p]
        cls.lib.RunRoundTrip.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint8),
                                        ctypes.POINTER(ctypes.c_uint8), ctypes.c_void_p]
        cls.lib.RunRoundTripV2.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint8),
                                          ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint64, ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    class HostResult(ctypes.Structure):
        _fields_ = [
            ("status", ctypes.c_uint64),
            ("attempted", ctypes.c_uint64),
            ("verified", ctypes.c_uint64),
            ("reads", ctypes.c_uint64),
            ("writes", ctypes.c_uint64),
            ("flushes", ctypes.c_uint64),
            ("grant_unchanged", ctypes.c_uint64),
            ("wrong_read_status", ctypes.c_uint64),
            ("wrong_generation_status", ctypes.c_uint64),
            ("record_tokens", ctypes.c_uint8 * 96),
            ("grant_token", ctypes.c_uint8 * 32),
            ("record_versions", ctypes.c_uint32 * 3),
            ("record_generations", ctypes.c_uint64 * 3),
        ]

    def test_runtime_token_is_record_identity_and_grant_is_unchanged(self):
        token = b"fedcba9876543210fedcba9876543210"
        token_arg = (ctypes.c_uint8 * 32).from_buffer_copy(token)
        out = self.HostResult()
        self.assertEqual(self.lib.RunWrite(token_arg, ctypes.byref(out)), 0)
        self.assertEqual((out.status, out.attempted, out.verified), (0, 1, 1))
        self.assertEqual((out.reads, out.writes, out.flushes), (2, 3, 3))
        self.assertEqual(out.grant_unchanged, 1)
        self.assertEqual(bytes(out.grant_token), b"0123456789abcdef0123456789abcdef")
        self.assertEqual(bytes(out.record_tokens), token * 3)

    def test_write_rejects_missing_or_malformed_token_before_io(self):
        malformed = (ctypes.c_uint8 * 32).from_buffer_copy(b"g" + b"0" * 31)
        for token in (None, malformed):
            out = self.HostResult()
            self.assertEqual(self.lib.RunWrite(token, ctypes.byref(out)), 0)
            self.assertEqual(out.status, 0x8000000000000000 | 26)
            self.assertEqual((out.attempted, out.reads, out.writes, out.flushes), (0, 0, 0, 0))

    def test_readback_requires_the_same_runtime_record_token(self):
        token = (ctypes.c_uint8 * 32).from_buffer_copy(b"fedcba9876543210fedcba9876543210")
        wrong = (ctypes.c_uint8 * 32).from_buffer_copy(b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        out = self.HostResult()
        self.assertEqual(self.lib.RunRoundTrip(token, token, wrong, ctypes.byref(out)), 0)
        self.assertEqual((out.status, out.verified, out.attempted), (0, 1, 1))
        self.assertEqual((out.reads, out.writes, out.flushes), (5, 3, 3))
        self.assertEqual(out.wrong_read_status, 0x8000000000000000 | 33)
        self.assertEqual(out.wrong_generation_status, 0x8000000000000000 | 33)

    def test_v2_write_persists_generation_in_all_three_records_without_changing_grant(self):
        token = (ctypes.c_uint8 * 32).from_buffer_copy(b"fedcba9876543210fedcba9876543210")
        out = self.HostResult()
        generation = 0x1020304050607080
        self.assertEqual(self.lib.RunWriteV2(token, generation, ctypes.byref(out)), 0)
        self.assertEqual((out.status, out.attempted, out.verified, out.grant_unchanged), (0, 1, 1, 1))
        self.assertEqual(list(out.record_versions), [2, 2, 2])
        self.assertEqual(list(out.record_generations), [generation] * 3)
        self.assertEqual(bytes(out.record_tokens), bytes(token) * 3)

    def test_userdata_write_rejects_missing_generation_before_io(self):
        token = (ctypes.c_uint8 * 32).from_buffer_copy(b"fedcba9876543210fedcba9876543210")
        out = self.HostResult()
        self.assertEqual(self.lib.RunWriteV2(token, 0, ctypes.byref(out)), 0)
        self.assertEqual(out.status, 0x8000000000000000 | 26)
        self.assertEqual((out.attempted, out.reads, out.writes, out.flushes), (0, 0, 0, 0))

    def test_v2_readback_checks_runtime_token_and_generation(self):
        token = (ctypes.c_uint8 * 32).from_buffer_copy(b"fedcba9876543210fedcba9876543210")
        wrong = (ctypes.c_uint8 * 32).from_buffer_copy(b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        out = self.HostResult()
        generation = 37
        self.assertEqual(self.lib.RunRoundTripV2(token, token, wrong, generation, ctypes.byref(out)), 0)
        self.assertEqual((out.status, out.verified, out.attempted), (0, 1, 1))
        self.assertEqual(out.wrong_read_status, 0x8000000000000000 | 33)
        self.assertEqual(out.wrong_generation_status, 0x8000000000000000 | 33)


if __name__ == "__main__":
    unittest.main()
