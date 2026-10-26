// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UfsIdentity.h"
#include "UfsProvenance.h"
#include "UfsLayout.h"

#define DALI_UFS_EXPDB_WIRE_BYTES 236U
#define DALI_UFS_USERDATA_LOG_WIRE_BYTES 240U
typedef struct {
  BOOLEAN Attempted, Prepared, AuthorizationAttempted;
  UINT64 Sequence;
  MT6991_UFS_SCRATCH_GRANT Proposal;
  DALI_UFS_PROVENANCE Provenance;
} DALI_UFS_GRANT_STATE;

// Called before physical controller access. Parse only explicit target schemas
// and compare independently measured RAM kernel/source and compiled token.
// Proposal.Enabled is a shape marker, never installed as the host live grant.
EFI_STATUS DaliUfsGrantPrepare (DALI_UFS_GRANT_STATE *State,
  CONST VOID *Wire, UINTN Bytes, CONST CHAR8 *Token, UINTN TokenBytes,
  CONST DALI_UFS_PROVENANCE *Provenance);
// Called once, under serialized admission, only after real GPT reads. Output
// is cleared on every failure; candidate/source and identity never self-copy
// from expected fields to fabricate observations. Geometry is indexed by LUN.
EFI_STATUS DaliUfsGrantAuthorize (DALI_UFS_GRANT_STATE *State,
  CONST DALI_UFS_IDENTITY *Identity, CONST DALI_UFS_PROVENANCE *Provenance,
  CONST MT6991_UFS_GEOMETRY *Geometry, CONST DALI_UFS_LAYOUT *Layout,
  BOOLEAN Closed, MT6991_UFS_SCRATCH_GRANT *Live);
