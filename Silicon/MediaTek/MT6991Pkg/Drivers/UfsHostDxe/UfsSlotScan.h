// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UfsLayout.h"

// Scan only the verified unixnu_logs extent and select a fully zero UEFI/XNU
// pair. Nonzero contents are conservatively occupied; no rotation is done.
EFI_STATUS DaliUfsSelectEmptyUserdataPair (DALI_UFS_LAYOUT *Layout, UINT64 Preferred,
  UINT32 ParentAlign, UINT32 ScsiAlign, DALI_UFS_LAYOUT_READ Read, VOID *Context);
