// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// A fully classified UEFI v2 record identity. Used only to bind a selected
// replacement to the record observed by the complete read-only slot scan.
typedef struct {
  BOOLEAN Valid;
  UINT64 Generation;
  UINT8 Token[32];
  UINT8 CandidateSha256[32];
  UINT8 SourceSha256[32];
  UINT8 BodySha256[32];
} DALI_UFS_LOG_V2_IDENTITY;
