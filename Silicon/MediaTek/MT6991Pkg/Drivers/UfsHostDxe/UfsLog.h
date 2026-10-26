// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Library/Mt6991UfsPolicy.h>

#define DALI_UFS_LOG_WINDOW_BYTES (1024U * 1024U)
#define DALI_UFS_LOG_WORK_BYTES (2U * DALI_UFS_LOG_WINDOW_BYTES)
#define DALI_UFS_LOG_OWNER_UEFI 1U
#define DALI_UFS_LOG_OWNER_XNU 2U

typedef struct {
  BOOLEAN Attempted, WriteSubmitted, CommitSynced, Verified;
  UINT32 BodyBytes;
  UINT8 BodySha256[32];
  // Borrowed from the caller's workspace, valid until it is reused/freed.
  CONST UINT8 *Body;
} DALI_UFS_LOG_RESULT;

// Consumer of an already runtime-authorized, bounded expdb scratch BlockIo.
// No grant is issued here. Workspace is two separate window-sized buffers.
// Write: zero-window check, attempt+sync, body+sync, commit+sync, full readback.
// ReadbackOnly: read/verify only, never initialize/replay an incomplete record.
// A result object is single-use; any I/O/hash failure stops the attempt.
EFI_STATUS DaliUfsLogRun (EFI_BLOCK_IO_PROTOCOL *Io,
  CONST MT6991_UFS_SCRATCH_GRANT *Grant, UINT32 Owner, UINT64 Sequence,
  BOOLEAN ReadbackOnly, CONST UINT8 *Body, UINTN BodyBytes,
  VOID *Workspace, UINTN WorkspaceBytes, DALI_UFS_LOG_RESULT *Result);
