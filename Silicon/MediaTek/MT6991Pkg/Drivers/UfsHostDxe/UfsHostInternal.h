// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
EFI_STATUS DaliUfsReadOnlyDiagnostics (EFI_HANDLE Controller, CONST CHAR8 *Token,
  CONST volatile UINT64 *CompletedSync);
EFI_STATUS DaliUfsBootStagePublish (EFI_HANDLE Controller);
EFI_STATUS DaliUfsBootStageCloseAdmission (VOID);
