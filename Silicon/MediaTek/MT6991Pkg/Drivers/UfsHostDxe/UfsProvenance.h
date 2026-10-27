// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define DALI_UFS_SOURCE_BYTES 180U
#define DALI_UFS_KERNEL_MAX_BYTES (128U * 1024U * 1024U)
typedef struct {
  BOOLEAN Ready;
  UINT8 CandidateSha256[32], SourceSha256[32];
} DALI_UFS_PROVENANCE;

// Kernel is the original complete RAM file, including all unloaded bytes.
// CompiledSource is firmware build metadata, never read from a write grant.
EFI_STATUS DaliUfsMeasureKernel (CONST VOID *Kernel, UINTN KernelBytes,
  CONST VOID *CompiledSource, UINTN SourceBytes, DALI_UFS_PROVENANCE *Observed);
// Select only the exact firmware-owned RAM filesystem; never search UFS files.
EFI_STATUS DaliUfsMeasureRamPayload (CONST VOID *CompiledSource, UINTN SourceBytes,
  DALI_UFS_PROVENANCE *Observed);
