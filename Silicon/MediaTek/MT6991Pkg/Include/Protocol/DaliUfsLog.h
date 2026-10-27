// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
extern EFI_GUID gDaliUfsLogProtocolGuid;
#define DALI_UFS_LOG_REVISION 3ULL
#define DALI_UFS_LOG_WRITE 1U
#define DALI_UFS_LOG_READBACK 2U
typedef struct _DALI_UFS_LOG_PROTOCOL DALI_UFS_LOG_PROTOCOL;
// RecordToken is exactly 32 lowercase hexadecimal bytes. WRITE requires a
// runtime token and Generation=0 (the host selects the next committed v2
// generation). READBACK uses Generation=0 for v1, or a nonzero v2 generation;
// v2 readback requires the exact record token. NULL token is legacy-v1 only.
typedef EFI_STATUS (EFIAPI *DALI_UFS_LOG_RUN)(DALI_UFS_LOG_PROTOCOL *This,
  UINT32 Mode, CONST UINT8 *RecordToken, UINT64 Generation);
// Optional, separate GUID: do not read beyond the existing diagnostic ABI.
// Single BSP/application-TPL invocation after Initialize succeeds and before
// Quiesce. A supplied token binds the record only; it never changes the grant.
// Readback never writes. Return status is not qualification.
struct _DALI_UFS_LOG_PROTOCOL { UINT64 Revision; DALI_UFS_LOG_RUN Run; };
