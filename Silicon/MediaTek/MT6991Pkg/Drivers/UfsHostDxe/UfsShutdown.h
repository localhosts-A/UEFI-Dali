// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/Mt6991UfsController.h>
#include <Library/Mt6991UfsPolicy.h>

typedef struct {
  EFI_STATUS Status;
  UINT32 Commands;
  UINT8 SyncedLuns;
  BOOLEAN Attempted, Completed;
} DALI_UFS_SHUTDOWN;

// The caller MUST close all public submissions first and keep them closed.
// Drain/retire any pending mapping, then use a private owned common page for
// bounded real SYNC(10). No PassThru entry is reopened. All private descriptors
// remain owned until the caller stops HCE, disconnects and releases DMA.
// A failed attempt is never replayed; the caller must still stop/quarantine.
EFI_STATUS DaliUfsDrainAndSync (
  MT6991_UFS_DMA *, MT6991_UFS_PENDING *, MT6991_UFS_GEOMETRY *,
  volatile UINT64 *CompletedSync, CONST MT6991_UFS_IO *, DALI_UFS_SHUTDOWN *
  );
