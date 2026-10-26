// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "UfsLogConsole.h"
#include "UfsScratch.h"

STATIC EFI_STATUS Snapshot (CONST DALI_RAM_CONSOLE *Ring, UINTN Capacity,
  UINT8 *Body, UINTN Maximum, UINTN *Bytes) {
  *Bytes = 0;
  if (Ring == NULL || Capacity == 0 || Capacity > 2U * 1024U * 1024U) { return EFI_INVALID_PARAMETER; }
  // Pool allocation and all BlockIo operations happen at application TPL.
  // Only this bounded volatile-ring copy masks interrupts.
  EFI_TPL Old = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  MemoryFence ();
  UINT32 Start = Ring->Start, Size = Ring->Size;
  EFI_STATUS Status = EFI_SUCCESS;
  if (Ring->Signature != 0x43474244 || Start >= Capacity || Size > Capacity ||
      (Size < Capacity && Start != Size)) { Status = EFI_COMPROMISED_DATA; }
  else if (Size == 0) { Status = EFI_NOT_FOUND; }
  else if (Size > Maximum) { Status = EFI_BUFFER_TOO_SMALL; }
  else {
    UINTN First = Size == Capacity ? Start : 0;
    for (UINTN I = 0; I < Size; ++I) { Body[I] = Ring->Data[(First + I) % Capacity]; }
    *Bytes = Size;
  }
  gBS->RestoreTPL (Old); return Status;
}

EFI_STATUS DaliUfsLogConsoleRun (DALI_UFS_LOG_CONSOLE *State,
  CONST MT6991_UFS_SCRATCH_GRANT *Live, UINT64 Sequence, BOOLEAN ReadbackOnly,
  CONST DALI_RAM_CONSOLE *Ring, UINTN Capacity) {
  if (State == NULL) { return EFI_INVALID_PARAMETER; }
  if (EfiGetCurrentTpl () != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  if (State->Attempted) { return EFI_ALREADY_STARTED; }
  State->Attempted = TRUE;
  ZeroMem (&State->Record, sizeof State->Record);
  EFI_BLOCK_IO_PROTOCOL *Io = NULL;
  State->Status = DaliUfsScratchOwnedIo (Live, &Io);
  if (State->Status != EFI_SUCCESS) { return State->Status; }
  // Boot-service allocation may dispatch shutdown. Freeze identity now; each
  // lower IO still checks its real live grant and rejects a later revocation.
  MT6991_UFS_SCRATCH_GRANT Grant = *Live;
  if (!Mt6991UfsScratchGrantValid (&Grant) || Grant.Target != MT6991_UFS_TARGET_EXPDB) {
    State->Status = EFI_SECURITY_VIOLATION; return State->Status;
  }
  UINTN Maximum = DALI_UFS_LOG_WINDOW_BYTES - 3 * Grant.BlockSize;
  VOID *Work = AllocateAlignedPages (EFI_SIZE_TO_PAGES (DALI_UFS_LOG_WORK_BYTES), EFI_PAGE_SIZE);
  UINT8 *Body = ReadbackOnly ? NULL : AllocatePool (Maximum);
  if (Work == NULL || (!ReadbackOnly && Body == NULL)) { State->Status = EFI_OUT_OF_RESOURCES; }
  else {
    UINTN Bytes = 0;
    State->Status = ReadbackOnly ? EFI_SUCCESS : Snapshot (Ring, Capacity, Body, Maximum, &Bytes);
    if (State->Status == EFI_SUCCESS) {
      State->Status = DaliUfsLogRun (Io, &Grant, DALI_UFS_LOG_OWNER_UEFI, Sequence,
        ReadbackOnly, Body, Bytes, Work, DALI_UFS_LOG_WORK_BYTES, &State->Record);
    }
  }
  // Preserve diagnostic flags/hash/length, never a pointer into freed work.
  State->Record.Body = NULL;
  if (Body != NULL) { ZeroMem (Body, Maximum); FreePool (Body); }
  if (Work != NULL) { ZeroMem (Work, DALI_UFS_LOG_WORK_BYTES); FreeAlignedPages (Work, EFI_SIZE_TO_PAGES (DALI_UFS_LOG_WORK_BYTES)); }
  return State->Status;
}
