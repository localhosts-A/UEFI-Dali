// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/BaseLib.h>
#include <Library/DaliConsole.h>
#include "UfsLog.h"

typedef struct {
  BOOLEAN Attempted;
  EFI_STATUS Status;
  DALI_UFS_LOG_RESULT Record;
} DALI_UFS_LOG_CONSOLE;

// Capture raw ring bytes without filtering. Ramoops continues unchanged.
// The exact authorized live grant, not a copied proposal, is required at entry.
// No automatic mode switch: ReadbackOnly cannot write or initialize any record.
EFI_STATUS DaliUfsLogConsoleRun (DALI_UFS_LOG_CONSOLE *State,
  CONST MT6991_UFS_SCRATCH_GRANT *Live, UINT64 Sequence,
  CONST UINT8 *RecordToken, BOOLEAN ReadbackOnly,
  CONST DALI_RAM_CONSOLE *Ring, UINTN Capacity);
