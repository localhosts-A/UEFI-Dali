// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
extern EFI_GUID gDaliUfsDiagnosticProtocolGuid;
// DXE entry does no hardware access. BDS supplies the validated live FDT after
// RAM payload preparation. This diagnostic always stops before launching RAM XNU.
typedef struct {
  EFI_STATUS (EFIAPI *Initialize)(CONST VOID *Fdt, EFI_HANDLE *Controller);
  EFI_STATUS (EFIAPI *Quiesce)(VOID);
} DALI_UFS_DIAGNOSTIC_PROTOCOL;
