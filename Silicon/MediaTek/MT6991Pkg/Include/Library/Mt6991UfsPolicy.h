// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/Mt6991UfsDmaLib.h>

typedef struct {
  UINT64 Blocks;
  UINT32 BlockSize;
  BOOLEAN Valid, WriteProtectionKnown, WriteProtected;
} MT6991_UFS_GEOMETRY;
#define MT6991_UFS_COMMAND_TIMEOUT_100NS 30000000ULL
EFI_STATUS Mt6991UfsBoundTimeout (UINT64 *Timeout);
typedef struct {
  // Populated only by a future verified layout/identity path. This layer checks
  // grant shape, current geometry/WP and command range; it cannot authenticate provenance.
  BOOLEAN Enabled;
  UINT8 Lun;
  CHAR8 Token[33];
  UINT8 DeviceBinding[32], CandidateSha256[32], SourceSha256[32], PartitionGuid[16];
  UINT32 BlockSize;
  UINT64 CapacityBlocks, BootStartLba, BootBlocks, CandidateBytes;
  UINT64 ScratchStartLba, ScratchBlocks, TailGuardBlocks;
} MT6991_UFS_SCRATCH_GRANT;
typedef struct {
  UINT8 *Trd, *Request, *Response, *Data;
  UINT32 Expected;
  UINTN ResponseBytes;
  UINT8 Transaction, Lun, Tag, Opcode;
} MT6991_UFS_PENDING;

EFI_STATUS Mt6991UfsReadOnlyCommand (CONST MT6991_UFS_GEOMETRY *Geometry,
  UINT8 Lun, CONST UINT8 *Cdb, UINTN CdbBytes, UINT32 ReadBytes);
EFI_STATUS Mt6991UfsScratchWriteCommand (CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST MT6991_UFS_SCRATCH_GRANT *Grant, UINT8 Lun, CONST UINT8 *Cdb, UINTN CdbBytes, UINT32 WriteBytes);
EFI_STATUS Mt6991UfsReadOnlyQuery (UINT8 Opcode, UINT8 Idn, UINT8 Index,
  UINT8 Selector, UINT32 Bytes, BOOLEAN InitAllowed);
// Called beneath PassThru, immediately before ringing slot 0. Only exact owned
// descriptors and one direction-matched contiguous data mapping are accepted;
// host-to-device data is restricted to a WRITE(10/16) with a valid scratch grant.
EFI_STATUS Mt6991UfsValidateTransfer (MT6991_UFS_DMA *, UINT64 Trl,
  CONST MT6991_UFS_GEOMETRY *, CONST MT6991_UFS_SCRATCH_GRANT *, BOOLEAN InitAllowed, MT6991_UFS_PENDING *);
// Caller observed DBR=0 and executed the DMA completion barrier first.
EFI_STATUS Mt6991UfsValidateResponse (MT6991_UFS_PENDING *, MT6991_UFS_GEOMETRY *, BOOLEAN *DataValid);
