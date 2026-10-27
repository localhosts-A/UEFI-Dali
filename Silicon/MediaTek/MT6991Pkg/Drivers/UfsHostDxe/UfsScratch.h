// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/Mt6991UfsPolicy.h>
#include "UfsLayout.h"
#include <Protocol/BlockIo.h>

// Invoked after real GPT inspection and before child installation. Until then
// Proposal is never the host's live write grant. Callback returns the verified
// live grant pointer, which all subsequent requests recheck for revocation.
// An enabled proposal with no authorizer is always refused.
typedef EFI_STATUS (*DALI_UFS_SCRATCH_AUTHORIZE)(VOID *Context,
  CONST MT6991_UFS_SCRATCH_GRANT *Proposal, CONST DALI_UFS_LAYOUT *Observed,
  CONST MT6991_UFS_SCRATCH_GRANT **Live);
EFI_STATUS DaliUfsScratchPublishChecked (EFI_HANDLE Controller,
  CONST MT6991_UFS_SCRATCH_GRANT *Proposal, CONST MT6991_UFS_GEOMETRY *Geometry,
  CONST volatile BOOLEAN *Closed, CONST volatile UINT64 *CompletedSync,
  DALI_UFS_SCRATCH_AUTHORIZE Authorize, VOID *Context);
// BSP only, under the host's TPL_NOTIFY admission lock. Close child admission
// first; NOT_READY forbids parent stop/disconnect until its one request returns.
EFI_STATUS DaliUfsScratchCloseAdmission (VOID);
// Call after closing host admission and before disconnecting the SCSI parent.
EFI_STATUS DaliUfsScratchUnpublish (VOID);
// Only the exact live, authorized grant may retrieve this driver's own child.
// Returned interface still rechecks media, revocation and ownership on every IO.
EFI_STATUS DaliUfsScratchOwnedIo (CONST MT6991_UFS_SCRATCH_GRANT *Live,
  EFI_BLOCK_IO_PROTOCOL **Io);
// Irreversible for this boot. Reads remain valid, all child writes refuse.
EFI_STATUS DaliUfsScratchSealReadOnly (CONST MT6991_UFS_SCRATCH_GRANT *Live);
