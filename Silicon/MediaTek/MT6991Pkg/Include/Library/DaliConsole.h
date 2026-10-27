// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Base.h>

typedef struct {
  volatile UINT32 Signature;
  volatile UINT32 Start;
  volatile UINT32 Size;
  volatile UINT8 Data[];
} DALI_RAM_CONSOLE;

STATIC inline VOID DaliConsoleInitialize (DALI_RAM_CONSOLE *Ring, UINT32 Capacity) {
  if (Ring->Signature != 0x43474244 || Ring->Start >= Capacity || Ring->Size > Capacity) {
    Ring->Start = 0;
    Ring->Size = 0;
    MemoryFence ();
    Ring->Signature = 0x43474244;
  }
}

STATIC inline VOID DaliConsoleAppend (
  DALI_RAM_CONSOLE *Ring, UINT32 Capacity, CONST UINT8 *Data, UINTN Bytes
  ) {
  DaliConsoleInitialize (Ring, Capacity);
  for (UINTN I = 0; I < Bytes; I++) {
    UINT32 At = Ring->Start;
    Ring->Data[At] = Data[I];
    Ring->Start = At + 1 == Capacity ? 0 : At + 1;
    if (Ring->Size < Capacity) { Ring->Size++; }
  }
  MemoryFence ();
}
