// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure sampling policy; include after Uefi.h. Now is monotonic nanoseconds.
#ifndef DALI_KEYPAD_TIMING_H
#define DALI_KEYPAD_TIMING_H

#define DALI_KEY_UP     1U
#define DALI_KEY_ENTER  2U
#define DALI_KEY_MS(N)  ((UINT64)(N) * 1000000ULL)

typedef struct {
  BOOLEAN Sample;
  BOOLEAN Stable;
  BOOLEAN Long;
  UINT64 ChangedAt;
  UINT64 HeldAt;
} DALI_KEY_STATE;

// Power and Volume down act once on a debounced press, with no hold repeat.
STATIC BOOLEAN DaliKeyPressSample (DALI_KEY_STATE *State, BOOLEAN Pressed, UINT64 Now) {
  if (Pressed != State->Sample) {
    State->Sample = Pressed;
    State->ChangedAt = Now;
  }
  if (Now - State->ChangedAt >= DALI_KEY_MS (20) && State->Stable != State->Sample) {
    State->Stable = State->Sample;
    return State->Stable;
  }
  return FALSE;
}

STATIC UINTN DaliKeySample (DALI_KEY_STATE *State, BOOLEAN Pressed, UINT64 Now) {
  if (Pressed != State->Sample) {
    State->Sample = Pressed;
    State->ChangedAt = Now;
  }
  if (Now - State->ChangedAt >= DALI_KEY_MS (20) && State->Stable != State->Sample) {
    State->Stable = State->Sample;
    if (State->Stable) {
      State->HeldAt = State->ChangedAt;
      State->Long = FALSE;
    } else if (!State->Long) {
      return DALI_KEY_UP;
    }
  }
  // A pending release must never be counted as a continuing hold, even if
  // drawing or another application delayed the next sample for a long time.
  if (Pressed && State->Stable && !State->Long && Now - State->HeldAt >= DALI_KEY_MS (700)) {
    State->Long = TRUE;
    return DALI_KEY_ENTER;
  }
  return 0;
}
#endif
