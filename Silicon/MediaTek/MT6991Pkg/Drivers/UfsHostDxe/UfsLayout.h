// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/Mt6991UfsPolicy.h>

#define DALI_UFS_GPT_MAX_ENTRIES 1024U
#define DALI_UFS_GPT_MAX_BYTES (128U * 1024U)
#define DALI_UFS_LAYOUT_WORK_BYTES (2U * 65536U + 2U * DALI_UFS_GPT_MAX_BYTES)

typedef EFI_STATUS (*DALI_UFS_LAYOUT_READ)(VOID *Context, UINT8 Lun, UINT64 Lba,
  UINTN Bytes, VOID *Buffer);
typedef struct {
  BOOLEAN Verified;
  UINT8 Lun;
  UINT32 BlockSize, EntryCount, EntryCrc32, PrimaryCrc32, BackupCrc32;
  UINT64 CapacityBlocks, BootStartLba, BootBlocks, ScratchStartLba, ScratchBlocks, TailGuardBlocks;
  // Physical log pair 1..4; userdata inspection initially copies the proposal,
  // then the bounded empty-pair scan replaces it with the selected pair.
  UINT64 Sequence;
  // RFC 4122 byte order, as in the host/XNU grant, rather than GPT's mixed endian GUID.
  UINT8 PartitionGuid[16];
  UINT32 Target;
} DALI_UFS_LAYOUT;

// Read exactly two headers and two bounded entry arrays; never retry a read.
// Caller supplies exclusive, stable media access and an appropriately aligned
// workspace. Geometry and write protection must be current device observations.
// Expected is an immutable host proposal, NOT a live write grant. This function
// only produces a layout observation. It does not establish candidate/source,
// device identity, AVB safety or evidence export, and cannot enable writes.
EFI_STATUS DaliUfsInspectScratchLayout (CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST MT6991_UFS_SCRATCH_GRANT *Expected, DALI_UFS_LAYOUT_READ Read, VOID *Context,
  VOID *Workspace, UINTN WorkspaceBytes, DALI_UFS_LAYOUT *Observed);
