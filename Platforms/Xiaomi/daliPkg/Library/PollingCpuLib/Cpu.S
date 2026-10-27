// CpuFlushTlb follows MdePkg/Library/BaseCpuLib/AArch64/CpuFlushTlb.S.
// Copyright (c) 2006 - 2009, Intel Corporation. All rights reserved.
// Portions copyright (c) 2008 - 2009, Apple Inc. All rights reserved.
// Portions copyright (c) 2011 - 2013, ARM Ltd. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause-Patent

.text
.p2align 2
GCC_ASM_EXPORT(CpuFlushTlb)
ASM_PFX(CpuFlushTlb):
  AARCH64_BTI(c)
  tlbi vmalle1
  dsb sy
  isb
  ret

// Early bring-up policy: let DXE recheck events without entering inherited
// platform idle. WFI wakeup is not yet validated on this MT6991 handoff.
GCC_ASM_EXPORT(CpuSleep)
ASM_PFX(CpuSleep):
  AARCH64_BTI(c)
  yield
  ret
