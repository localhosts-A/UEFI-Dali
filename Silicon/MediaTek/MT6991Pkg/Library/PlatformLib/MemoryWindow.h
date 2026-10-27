// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

typedef struct {
  UINT64 Base;
  UINT64 End;
} DALI_RAM_RANGE;

#define DALI_WINDOW_ALIGNMENT  0x200000ULL
// Include room for DarwinBoot's kernel-base offset within a 2 MiB block.
#define DALI_WINDOW_MINIMUM    0x10200000ULL

STATIC BOOLEAN DaliAddRange (
  DALI_RAM_RANGE *Ranges, UINTN *Count, UINTN Capacity, UINT64 Base, UINT64 Size
  ) {
  if (Size == 0) { return TRUE; }
  if (*Count >= Capacity || Base > MAX_UINT64 - Size) { return FALSE; }
  Ranges[*Count].Base = Base;
  Ranges[*Count].End = Base + Size;
  (*Count)++;
  return TRUE;
}

// Find the largest aligned gap in a DRAM bank below 4 GiB. Reservations need
// not be sorted or disjoint. No allocation or writes to candidate RAM occur.
STATIC BOOLEAN DaliFindGap (
  CONST DALI_RAM_RANGE *Banks, UINTN BankCount,
  CONST DALI_RAM_RANGE *Blocked, UINTN BlockedCount,
  UINT64 Alignment, UINT64 Minimum,
  UINT64 *Base, UINT64 *Size
  ) {
  *Base = 0;
  *Size = 0;
  if (Alignment < 0x1000 || Alignment > 0x100000000ULL || (Alignment & (Alignment - 1)) || Minimum == 0) { return FALSE; }
  for (UINTN Bank = 0; Bank < BankCount; Bank++) {
    UINT64 Start = Banks[Bank].Base < 0x80000000ULL ? 0x80000000ULL : Banks[Bank].Base;
    UINT64 Limit = Banks[Bank].End > 0x100000000ULL ? 0x100000000ULL : Banks[Bank].End;
    if (Start >= Limit) { continue; }
    Start = (Start + Alignment - 1) & ~(Alignment - 1);
    while (Start < Limit) {
      UINT64 End = Limit;
      UINT64 Next = Start;
      for (UINTN Index = 0; Index < BlockedCount; Index++) {
        if (Blocked[Index].Base <= Start && Start < Blocked[Index].End) {
          if (Blocked[Index].End > Next) { Next = Blocked[Index].End; }
        } else if (Blocked[Index].Base > Start && Blocked[Index].Base < End) {
          End = Blocked[Index].Base;
        }
      }
      if (Next != Start) {
        if (Next >= Limit) { break; }
        Start = (Next + Alignment - 1) & ~(Alignment - 1);
        continue;
      }
      End &= ~(Alignment - 1);
      if (End > Start && End - Start >= Minimum && End - Start > *Size) {
        *Base = Start;
        *Size = End - Start;
      }
      // End may equal Start when a reservation begins within this page/block.
      // Advance past it on the next iteration, without rounding down a hole.
      Start = End > Start ? End : Start + Alignment;
    }
  }
  return *Size != 0;
}

STATIC BOOLEAN DaliChooseWindow (
  CONST DALI_RAM_RANGE *Banks, UINTN BankCount,
  CONST DALI_RAM_RANGE *Blocked, UINTN BlockedCount, UINT64 *Base, UINT64 *Size
  ) {
  return DaliFindGap (Banks, BankCount, Blocked, BlockedCount,
    DALI_WINDOW_ALIGNMENT, DALI_WINDOW_MINIMUM, Base, Size);
}
