// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UfsLayout.h"
#include "UfsLogSlotScan.h"

// Scan only the verified unixnu_logs extent and select a fully zero UEFI/XNU
// pair. Nonzero contents are conservatively occupied; no rotation is done.
EFI_STATUS DaliUfsSelectEmptyUserdataPair (DALI_UFS_LAYOUT *Layout, UINT64 Preferred,
  UINT32 ParentAlign, UINT32 ScsiAlign, DALI_UFS_LAYOUT_READ Read, VOID *Context);
// Reuse an already completed full slot classification instead of reading the
// partition again; empty pairs are preferred, then only the oldest committed
// UEFI v2 record with an empty XNU sibling can be replaced.
EFI_STATUS DaliUfsSelectEmptyUserdataPairFromStates (DALI_UFS_LAYOUT *Layout,
  UINT64 Preferred, CONST DALI_UFS_LOG_SLOT_STATE Slots[DALI_UFS_LOG_SLOT_COUNT]);
EFI_STATUS DaliUfsSelectUserdataPairFromScan (DALI_UFS_LAYOUT *Layout,
  UINT64 Preferred, DALI_UFS_LOG_SLOT_SCAN_RESULT *Result);
