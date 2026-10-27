// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UfsLayout.h"
#include "UfsLogRecord.h"

#define DALI_UFS_LOG_SLOT_COUNT 8U

typedef enum {
  DaliUfsLogSlotEmpty = 0,
  DaliUfsLogSlotValidV1,
  DaliUfsLogSlotValidV2,
  DaliUfsLogSlotInterruptedV2,
  DaliUfsLogSlotUnknown
} DALI_UFS_LOG_SLOT_STATE;

typedef struct {
  DALI_UFS_LOG_SLOT_STATE Slots[DALI_UFS_LOG_SLOT_COUNT];
  // Pair is consistent only when empty or when a v2 UEFI record has an empty
  // XNU sibling or a matching v2 XNU record. This does not grant reuse.
  BOOLEAN PairConsistent[4];
  BOOLEAN HasInconsistentPair;
  // Generation comes only from fully verified committed UEFI v2 records.
  BOOLEAN HasCommittedV2;
  UINT32 CommittedUefiV2Count;
  UINT64 MaxGeneration;
  UINT64 NextGeneration;
  BOOLEAN NextGenerationValid;
  DALI_UFS_LOG_V2_IDENTITY UefiV2[4];
  // Filled by the selector from this same complete scan result.
  BOOLEAN RotationRequired;
  UINT64 SelectedSequence;
  DALI_UFS_LOG_V2_IDENTITY Victim;
} DALI_UFS_LOG_SLOT_SCAN_RESULT;

// Read and classify the eight owner slots without changing write authority.
// Expected is the immutable proposal, used only to bind records to this device
// and observed partition geometry. Unknown and interrupted slots remain occupied.
EFI_STATUS DaliUfsClassifyUserdataLogSlots (CONST DALI_UFS_LAYOUT *Layout,
  CONST MT6991_UFS_SCRATCH_GRANT *Expected, UINT32 ParentAlign, UINT32 ScsiAlign,
  DALI_UFS_LAYOUT_READ Read, VOID *Context, DALI_UFS_LOG_SLOT_SCAN_RESULT *Result);
