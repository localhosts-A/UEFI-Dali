// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "UfsSlotScan.h"

STATIC BOOLEAN ValidAlignment (UINT32 Align) {
  return Align <= EFI_PAGE_SIZE && (Align == 0 || (Align & (Align - 1)) == 0);
}

STATIC BOOLEAN ZeroBytes (CONST UINT8 *Bytes, UINTN Length) {
  UINT8 Any = 0;
  for (UINTN Index = 0; Index < Length; ++Index) { Any |= Bytes[Index]; }
  return Any == 0;
}

EFI_STATUS DaliUfsSelectEmptyUserdataPairFromStates (DALI_UFS_LAYOUT *Layout,
  UINT64 Preferred, CONST DALI_UFS_LOG_SLOT_STATE Slots[DALI_UFS_LOG_SLOT_COUNT]) {
  STATIC CONST UINT8 UserdataLogsGuid[16] = {
    0xcd, 0xe9, 0xab, 0x32, 0x18, 0xd8, 0x47, 0x77,
    0x8c, 0xed, 0x69, 0x1e, 0x7c, 0xd5, 0xc6, 0x36
  };
  if (Layout == NULL || Slots == NULL || !Layout->Verified ||
      Layout->Target != MT6991_UFS_TARGET_USERDATA_LOGS || Layout->Lun != 2 ||
      Layout->BlockSize != 4096 || Layout->CapacityBlocks != 64577536ULL ||
      Layout->BootStartLba != 64565240ULL || Layout->BootBlocks != 8192ULL ||
      Layout->ScratchBlocks != MT6991_UFS_DATA_LOG_SLOT_BYTES / Layout->BlockSize ||
      Preferred == 0 || Preferred > 4 || Layout->Sequence != Preferred ||
      CompareMem (Layout->PartitionGuid, UserdataLogsGuid, sizeof UserdataLogsGuid)) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT64 PartitionEnd = Layout->BootStartLba + Layout->BootBlocks;
  if (PartitionEnd < Layout->BootStartLba || PartitionEnd > Layout->CapacityBlocks) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT64 Selected = 0;
  for (UINT64 Offset = 0; Offset < 4; ++Offset) {
    UINT64 Sequence = ((Preferred - 1 + Offset) % 4) + 1;
    UINTN First = (UINTN)((Sequence - 1) * 2);
    if (Slots[First] == DaliUfsLogSlotEmpty && Slots[First + 1] == DaliUfsLogSlotEmpty) {
      Selected = Sequence; break;
    }
  }
  if (Selected == 0) { return EFI_OUT_OF_RESOURCES; }
  UINT64 SlotBlocks = Layout->ScratchBlocks;
  UINT64 SlotOffset = (Selected - 1) * 2 * SlotBlocks;
  Layout->Sequence = Selected;
  Layout->ScratchStartLba = Layout->BootStartLba + SlotOffset;
  Layout->TailGuardBlocks = Layout->BootBlocks - SlotOffset - SlotBlocks;
  if (Layout->ScratchStartLba < Layout->BootStartLba ||
      Layout->ScratchStartLba > PartitionEnd || SlotBlocks > PartitionEnd - Layout->ScratchStartLba) {
    return EFI_SECURITY_VIOLATION;
  }
  return EFI_SUCCESS;
}

// Called under the scratch driver's admission fence. The callback must retain
// its media/closure checks on every read; this helper independently bounds all
// requests to the verified unixnu_logs GPT extent and 1 MiB DMA transfer size.
EFI_STATUS DaliUfsSelectEmptyUserdataPair (DALI_UFS_LAYOUT *Layout, UINT64 Preferred,
  UINT32 ParentAlign, UINT32 ScsiAlign, DALI_UFS_LAYOUT_READ Read, VOID *Context) {
  STATIC CONST UINT8 UserdataLogsGuid[16] = {
    0xcd, 0xe9, 0xab, 0x32, 0x18, 0xd8, 0x47, 0x77,
    0x8c, 0xed, 0x69, 0x1e, 0x7c, 0xd5, 0xc6, 0x36
  };
  if (Layout == NULL || Read == NULL || !Layout->Verified ||
      Layout->Target != MT6991_UFS_TARGET_USERDATA_LOGS || Layout->Lun != 2 ||
      Layout->BlockSize != 4096 || Layout->CapacityBlocks != 64577536ULL ||
      Layout->BootStartLba != 64565240ULL || Layout->BootBlocks != 8192ULL ||
      Layout->ScratchBlocks != MT6991_UFS_DATA_LOG_SLOT_BYTES / Layout->BlockSize ||
      Preferred == 0 || Preferred > 4 || Layout->Sequence != Preferred ||
      CompareMem (Layout->PartitionGuid, UserdataLogsGuid, sizeof UserdataLogsGuid)) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT64 PartitionEnd = Layout->BootStartLba + Layout->BootBlocks;
  if (PartitionEnd < Layout->BootStartLba || PartitionEnd > Layout->CapacityBlocks) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT32 Align = ParentAlign > ScsiAlign ? ParentAlign : ScsiAlign;
  if (!ValidAlignment (Align)) { return EFI_UNSUPPORTED; }
  if (Align < EFI_PAGE_SIZE) { Align = EFI_PAGE_SIZE; }
  VOID *Buffer = AllocateAlignedPages (EFI_SIZE_TO_PAGES (MT6991_UFS_DMA_MAX_TRANSFER), Align);
  if (Buffer == NULL) { return EFI_OUT_OF_RESOURCES; }
  BOOLEAN Empty[8];
  DALI_UFS_LOG_SLOT_STATE States[DALI_UFS_LOG_SLOT_COUNT];
  ZeroMem (Empty, sizeof Empty);
  UINT64 SlotBlocks = Layout->ScratchBlocks;
  UINT64 ChunkBlocks = MT6991_UFS_DMA_MAX_TRANSFER / Layout->BlockSize;
  if (ChunkBlocks == 0 || ChunkBlocks > SlotBlocks) {
    FreeAlignedPages (Buffer, EFI_SIZE_TO_PAGES (MT6991_UFS_DMA_MAX_TRANSFER));
    return EFI_BAD_BUFFER_SIZE;
  }
  EFI_STATUS Status = EFI_SUCCESS;
  for (UINTN Slot = 0; Slot < ARRAY_SIZE (Empty) && Status == EFI_SUCCESS; ++Slot) {
    Empty[Slot] = TRUE;
    UINT64 SlotOffset = Slot * SlotBlocks;
    if (SlotOffset > Layout->BootBlocks || SlotBlocks > Layout->BootBlocks - SlotOffset) {
      Status = EFI_SECURITY_VIOLATION;
      break;
    }
    for (UINT64 Offset = 0; Offset < SlotBlocks; Offset += ChunkBlocks) {
      UINT64 Count = SlotBlocks - Offset;
      if (Count > ChunkBlocks) { Count = ChunkBlocks; }
      UINT64 Relative = SlotOffset + Offset;
      UINT64 Lba = Layout->BootStartLba + Relative;
      UINTN Bytes = (UINTN)(Count * Layout->BlockSize);
      if (Relative > Layout->BootBlocks || Count > Layout->BootBlocks - Relative ||
          Lba < Layout->BootStartLba || Lba > PartitionEnd || Count > PartitionEnd - Lba ||
          Bytes == 0 || Bytes > MT6991_UFS_DMA_MAX_TRANSFER || Bytes % Layout->BlockSize != 0) {
        Status = EFI_SECURITY_VIOLATION;
        break;
      }
      Status = Read (Context, Layout->Lun, Lba, Bytes, Buffer);
      if (Status != EFI_SUCCESS) { break; }
      if (!ZeroBytes (Buffer, Bytes)) { Empty[Slot] = FALSE; }
    }
  }
  FreeAlignedPages (Buffer, EFI_SIZE_TO_PAGES (MT6991_UFS_DMA_MAX_TRANSFER));
  if (Status != EFI_SUCCESS) { return Status; }
  for (UINTN Slot = 0; Slot < ARRAY_SIZE (States); ++Slot) {
    States[Slot] = Empty[Slot] ? DaliUfsLogSlotEmpty : DaliUfsLogSlotUnknown;
  }
  return DaliUfsSelectEmptyUserdataPairFromStates (Layout, Preferred, States);
}
