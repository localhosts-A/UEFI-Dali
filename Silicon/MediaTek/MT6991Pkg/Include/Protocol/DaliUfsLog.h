// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
extern EFI_GUID gDaliUfsLogProtocolGuid;
#define DALI_UFS_LOG_REVISION 1ULL
#define DALI_UFS_LOG_WRITE 1U
#define DALI_UFS_LOG_READBACK 2U
typedef struct _DALI_UFS_LOG_PROTOCOL DALI_UFS_LOG_PROTOCOL;
typedef EFI_STATUS (EFIAPI *DALI_UFS_LOG_RUN)(DALI_UFS_LOG_PROTOCOL *This, UINT32 Mode);
// Optional, separate GUID: do not read beyond the existing diagnostic ABI.
// Single BSP/application-TPL invocation per boot, after Initialize succeeds and
// before Quiesce. Readback never writes. Return status is not qualification.
struct _DALI_UFS_LOG_PROTOCOL { UINT64 Revision; DALI_UFS_LOG_RUN Run; };
