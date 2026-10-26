// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/Mt6991UfsPolicy.h>

// Internal consumer of an already authenticated host grant, not an authorization
// API. The host currently has no grant producer, so publication stays disabled.
EFI_STATUS DaliUfsScratchPublish (EFI_HANDLE Controller,
  CONST MT6991_UFS_SCRATCH_GRANT *Grant, CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST volatile BOOLEAN *Closed, CONST volatile UINT64 *CompletedSync);
// BSP only, under the host's TPL_NOTIFY admission lock. Close child admission
// first; NOT_READY forbids parent stop/disconnect until its one request returns.
EFI_STATUS DaliUfsScratchCloseAdmission (VOID);
// Call after closing host admission and before disconnecting the SCSI parent.
EFI_STATUS DaliUfsScratchUnpublish (VOID);
