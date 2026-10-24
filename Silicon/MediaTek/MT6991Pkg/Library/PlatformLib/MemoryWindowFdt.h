// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "MemoryWindow.h"

STATIC UINT64 DaliFdtCells (CONST UINT32 *P, INT32 Count) {
  UINT64 Value = 0;
  for (INT32 I = 0; I < Count; I++) { Value = (Value << 32) | Fdt32ToCpu (P[I]); }
  return Value;
}

STATIC BOOLEAN DaliAddNodeRanges (
  CONST VOID *Fdt, INT32 Node, DALI_RAM_RANGE *Ranges, UINTN *Count, UINTN Capacity
  ) {
  INT32 Length, Parent = FdtParentOffset (Fdt, Node);
  INT32 Ac = FdtAddressCells (Fdt, Parent), Sc = FdtSizeCells (Fdt, Parent);
  CONST UINT32 *Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  for (INT32 Bus = Parent; Bus >= 0; Bus = FdtParentOffset (Fdt, Bus)) {
    INT32 RangeLength;
    if (FdtGetProp (Fdt, Bus, "ranges", &RangeLength) != NULL && RangeLength != 0) { return FALSE; }
  }
  if (Reg == NULL) { return TRUE; } // Dynamic requests are placed in the second pass.
  if (Ac < 1 || Ac > 2 || Sc < 1 || Sc > 2 || Length <= 0 || Length % (4 * (Ac + Sc))) { return FALSE; }
  for (INT32 I = 0; I < Length / 4; I += Ac + Sc) {
    if (!DaliAddRange (Ranges, Count, Capacity, DaliFdtCells (Reg + I, Ac), DaliFdtCells (Reg + I + Ac, Sc))) { return FALSE; }
  }
  return TRUE;
}

STATIC BOOLEAN DaliReserveDynamicPool (
  CONST VOID *Fdt, INT32 Node, CONST DALI_RAM_RANGE *Banks, UINTN BankCount,
  DALI_RAM_RANGE *Blocked, UINTN *BlockedCount, UINTN Capacity
  ) {
  INT32 Length, Parent = FdtParentOffset (Fdt, Node);
  INT32 Ac = FdtAddressCells (Fdt, Parent), Sc = FdtSizeCells (Fdt, Parent);
  CONST UINT32 *Property = FdtGetProp (Fdt, Node, "reg", &Length);
  DALI_RAM_RANGE Allowed[16], PoolBanks[64];
  UINTN AllowedCount = 0, PoolBankCount = 0;
  UINT64 Bytes, Alignment = 0x1000, Base, Available;
  if (Property != NULL) { return TRUE; }
  Property = FdtGetProp (Fdt, Node, "size", &Length);
  if (Property == NULL) {
    return FdtGetProp (Fdt, Node, "alloc-ranges", &Length) == NULL;
  }
  if (Sc < 1 || Sc > 2 || Ac < 1 || Ac > 2 || Length != 4 * Sc) { return FALSE; }
  Bytes = DaliFdtCells (Property, Sc);
  if (Bytes == 0 || Bytes > 0x100000000ULL - 0xFFF) { return FALSE; }
  Bytes = (Bytes + 0xFFF) & ~0xFFFULL;
  Property = FdtGetProp (Fdt, Node, "alignment", &Length);
  if (Property != NULL) {
    if (Length != 4 * Sc) { return FALSE; }
    Alignment = DaliFdtCells (Property, Sc);
    if (Alignment == 0 || (Alignment & (Alignment - 1))) { return FALSE; }
    if (Alignment < 0x1000) { Alignment = 0x1000; }
  }
  Property = FdtGetProp (Fdt, Node, "alloc-ranges", &Length);
  if (Property == NULL) {
    for (UINTN I = 0; I < BankCount; I++) {
      if (!DaliAddRange (Allowed, &AllowedCount, ARRAY_SIZE (Allowed), Banks[I].Base, Banks[I].End - Banks[I].Base)) { return FALSE; }
    }
  } else {
    if (Length <= 0 || Length % (4 * (Ac + Sc))) { return FALSE; }
    for (INT32 I = 0; I < Length / 4; I += Ac + Sc) {
      if (!DaliAddRange (Allowed, &AllowedCount, ARRAY_SIZE (Allowed),
          DaliFdtCells (Property + I, Ac), DaliFdtCells (Property + I + Ac, Sc))) { return FALSE; }
    }
  }
  for (UINTN I = 0; I < BankCount; I++) {
    for (UINTN J = 0; J < AllowedCount; J++) {
      UINT64 Lo = Banks[I].Base > Allowed[J].Base ? Banks[I].Base : Allowed[J].Base;
      UINT64 End = Banks[I].End < Allowed[J].End ? Banks[I].End : Allowed[J].End;
      if (End > Lo && !DaliAddRange (PoolBanks, &PoolBankCount, ARRAY_SIZE (PoolBanks), Lo, End - Lo)) { return FALSE; }
    }
  }
  if (!DaliFindGap (PoolBanks, PoolBankCount, Blocked, *BlockedCount, Alignment, Bytes, &Base, &Available)) { return FALSE; }
  return DaliAddRange (Blocked, BlockedCount, Capacity, Base, Bytes);
}

// Parse the incoming LK FDT before adding any new memory HOB. Fixed contains
// all existing firmware, stack, heap, log and framebuffer allocations.
STATIC BOOLEAN DaliWindowFromFdt (
  CONST VOID *Fdt, UINT64 BootFdt, CONST DALI_RAM_RANGE *Fixed, UINTN FixedCount,
  UINT64 *Base, UINT64 *Size, UINTN *Exclusions
  ) {
  DALI_RAM_RANGE Banks[16], Blocked[256];
  UINTN BankCount = 0, BlockedCount = 0;
  UINT64 Lo, Bytes;
  INT32 Node, Depth, Length, Reserved;
  CONST UINT32 *Property;
  if (FdtCheckHeader (Fdt) || !DaliAddNodeRanges (Fdt, FdtPathOffset (Fdt, "/memory"),
      Banks, &BankCount, ARRAY_SIZE (Banks)) || BankCount == 0) { return FALSE; }
  for (UINTN I = 0; I < FixedCount; I++) {
    if (Fixed[I].End < Fixed[I].Base || !DaliAddRange (Blocked, &BlockedCount,
        ARRAY_SIZE (Blocked), Fixed[I].Base, Fixed[I].End - Fixed[I].Base)) { return FALSE; }
  }
  if (!DaliAddRange (Blocked, &BlockedCount, ARRAY_SIZE (Blocked), BootFdt, FdtTotalSize (Fdt))) { return FALSE; }
  INTN ReserveCount = FdtGetNumberOfReserveMapEntries (Fdt);
  if (ReserveCount < 0) { return FALSE; }
  for (INTN I = 0; I < ReserveCount; I++) {
    if (FdtGetReserveMapEntry (Fdt, I, &Lo, &Bytes) ||
        !DaliAddRange (Blocked, &BlockedCount, ARRAY_SIZE (Blocked), Lo, Bytes)) { return FALSE; }
  }
  Reserved = FdtPathOffset (Fdt, "/reserved-memory");
  if (Reserved < 0) { return FALSE; }
  Depth = 0;
  for (Node = FdtNextNode (Fdt, Reserved, &Depth); Node >= 0 && Depth > 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    if (!DaliAddNodeRanges (Fdt, Node, Blocked, &BlockedCount, ARRAY_SIZE (Blocked))) { return FALSE; }
  }
  Depth = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    if (FdtStringListSearch (Fdt, Node, "compatible", "pkvm,protected-region") >= 0) {
      if (FdtGetProp (Fdt, Node, "reg", &Length) == NULL ||
          !DaliAddNodeRanges (Fdt, Node, Blocked, &BlockedCount, ARRAY_SIZE (Blocked))) { return FALSE; }
    }
  }
  Node = FdtPathOffset (Fdt, "/chosen");
  Property = FdtGetProp (Fdt, Node, "linux,initrd-start", &Length);
  if (Property != NULL) {
    if (Length != 4 && Length != 8) { return FALSE; }
    Lo = DaliFdtCells (Property, Length / 4);
    Property = FdtGetProp (Fdt, Node, "linux,initrd-end", &Length);
    if (Property == NULL || (Length != 4 && Length != 8)) { return FALSE; }
    Bytes = DaliFdtCells (Property, Length / 4);
    if (Bytes < Lo || !DaliAddRange (Blocked, &BlockedCount, ARRAY_SIZE (Blocked), Lo, Bytes - Lo)) { return FALSE; }
  } else if (FdtGetProp (Fdt, Node, "linux,initrd-end", &Length) != NULL) {
    return FALSE;
  }
  // Honor size-only Linux pool requests without starting their consumers.
  // The chosen placements stay excluded for this entire firmware/XNU boot.
  Depth = 0;
  for (Node = FdtNextNode (Fdt, Reserved, &Depth); Node >= 0 && Depth > 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    if (!DaliReserveDynamicPool (Fdt, Node, Banks, BankCount, Blocked, &BlockedCount, ARRAY_SIZE (Blocked))) { return FALSE; }
  }
  *Exclusions = BlockedCount;
  return DaliChooseWindow (Banks, BankCount, Blocked, BlockedCount, Base, Size);
}
