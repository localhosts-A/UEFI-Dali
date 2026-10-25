// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Library/MemoryMapLib.h>

// Dali diagnostic layout; host preflight checks FD/stack before boot. SEC checks
// the incoming FDT again before copying it or allocating the DXE heap.
// Keep in sync with Resources/Configs/dali.toml and BootShim/DaliShim.S.
#define DALI_FD_BASE        0xC1000000ULL
#define DALI_FD_SIZE        0x04000000ULL
#define DALI_STACK_BASE     0xC5000000ULL
#define DALI_STACK_SIZE     0x00040000ULL
#define DALI_FDT_BASE       0xC5080000ULL
#define DALI_FDT_SIZE       0x00100000ULL
#define DALI_HEAP_BASE      0xC5200000ULL
#define DALI_HEAP_SIZE      0x08000000ULL
#define DALI_PSTORE_BASE    0xAEC00000ULL
#define DALI_PSTORE_SIZE    0x00410000ULL
// Verified ramoops layout: 0x410000 - console(2M) - pmsg(2M) - ftrace(4K).
#define DALI_CONSOLE_BASE   (DALI_PSTORE_BASE + 0xF000)
#define DALI_CONSOLE_SIZE   0x00200000U
#define DALI_FB_BASE        0xFC990000ULL
#define DALI_FB_SIZE        0x03660000ULL
#define DALI_GPIO_BASE      0x1002D000ULL
#define DALI_WDT_BASE       0x1C010000ULL
#define DALI_PMIF_BASE      0x1C01A000ULL

EFI_STATUS Mt6991PlatformInitialize (IN UINTN BootFdt);
VOID Mt6991GetMemoryMap (OUT EFI_MEMORY_REGION_DESCRIPTOR **Map, OUT UINT8 *Count);
VOID Mt6991LogRegisters (VOID);
VOID Mt6991Reset (VOID);
VOID Mt6991WarmReset (VOID);
EFI_STATUS Mt6991ProbeUfs (IN CONST VOID *Fdt);
EFI_STATUS Mt6991PrepareResetQualification (IN CONST VOID *Fdt, IN CONST CHAR8 *RunToken);
EFI_STATUS Mt6991PrepareFastbootRecovery (
  IN CONST VOID *Fdt,
  IN UINT32 TimeoutSeconds,
  IN CONST CHAR8 *RunToken
  );
