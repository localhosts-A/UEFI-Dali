// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/IoLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/DaliConsole.h>
STATIC VOID Log (CONST CHAR8 *Text);
STATIC VOID LogTimed (CONST CHAR8 *Text);
STATIC VOID LogMemoryNode (INT32 Node, UINTN Count);
#define DALI_MEMORY_TRACE(Message) LogTimed ("DALI: RAM trace=" Message)
#define DALI_MEMORY_NODE_TRACE(Node, Count) LogMemoryNode (Node, Count)
#include "MemoryWindowFdt.h"

// Expose the validated diagnostic arena plus one FDT-derived XNU window.
STATIC EFI_MEMORY_REGION_DESCRIPTOR mMap[] = {
  {"UEFI FD", DALI_FD_BASE, DALI_FD_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI Stack", DALI_STACK_BASE, DALI_STACK_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Boot FDT", DALI_FDT_BASE, DALI_FDT_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, LdData, WRITE_BACK},
  {"DXE Heap", DALI_HEAP_BASE, DALI_HEAP_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
  {"Ramoops", DALI_PSTORE_BASE, DALI_PSTORE_SIZE, AddMem, MEM_RES, PRESENT | INITIALIZED, Reserv, NS_DEVICE},
  {"GIC Distributor", 0x0C400000, 0x40000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GIC Redistributors", 0x0C440000, 0x200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display Reserved", DALI_FB_BASE, DALI_FB_SIZE, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"GPIO input probe", DALI_GPIO_BASE, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Inherited RGU watchdog", DALI_WDT_BASE, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Inherited PMIF SPMI-M", DALI_PMIF_BASE, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  // Mapping alone performs no controller access. The opt-in probe validates
  // live FDT bindings and inherited clock status before reading the HCI.
  {"UFS diagnostic HCI", 0x16810000, 0x3000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"UFS AO clock status", 0x168A0000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"DSI state probe", 0x32490000, 0x30000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL0 state probe", 0x32830000, 0xA0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL1 state probe", 0x32C50000, 0x80000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"DISP0 mutex", 0x32020000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"DISP1 mutex", 0x32420000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL0 mutex", 0x32820000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL1 mutex", 0x32C20000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"XNU RAM", 0, 0, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
};

VOID Mt6991GetMemoryMap (OUT EFI_MEMORY_REGION_DESCRIPTOR **Map, OUT UINT8 *Count) {
  *Map = mMap;
  *Count = ARRAY_SIZE (mMap);
}

STATIC VOID Log (CONST CHAR8 *Text) {
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
}

STATIC VOID LogTimed (CONST CHAR8 *Text) {
  UINT64 Ticks;
  CHAR8 Message[192];
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (Ticks));
  AsciiSPrint (Message, sizeof Message, "%a ticks=%lx\n", Text, Ticks);
  Log (Message);
}

STATIC VOID LogMemoryNode (INT32 Node, UINTN Count) {
  UINT64 Ticks;
  CHAR8 Message[128];
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (Ticks));
  AsciiSPrint (Message, sizeof Message, "DALI: RAM static node=%d count=%lu ticks=%lx\n",
    Node, (UINT64)Count, Ticks);
  Log (Message);
}

VOID Mt6991LogRegisters (VOID) {
  UINT64 El, Mpidr, Freq, Esr, Far, Elr;
  CHAR8 Buffer[256];
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  __asm__ volatile ("mrs %0, MPIDR_EL1" : "=r" (Mpidr));
  __asm__ volatile ("mrs %0, CNTFRQ_EL0" : "=r" (Freq));
  if (El == 8) {
    __asm__ volatile ("mrs %0, ESR_EL2" : "=r" (Esr));
    __asm__ volatile ("mrs %0, FAR_EL2" : "=r" (Far));
    __asm__ volatile ("mrs %0, ELR_EL2" : "=r" (Elr));
  } else {
    __asm__ volatile ("mrs %0, ESR_EL1" : "=r" (Esr));
    __asm__ volatile ("mrs %0, FAR_EL1" : "=r" (Far));
    __asm__ volatile ("mrs %0, ELR_EL1" : "=r" (Elr));
  }
  AsciiSPrint (Buffer, sizeof Buffer, "DALI: EL%lu MPIDR=%lx CNTFRQ=%lu ESR=%lx FAR=%lx ELR=%lx\n",
    El >> 2, Mpidr, Freq, Esr, Far, Elr);
  Log (Buffer);
}

VOID Mt6991Reset (VOID) {
  ARM_SMC_ARGS Args = {0};
  Log ("DALI: PSCI SYSTEM_RESET\n");
  MemoryFence ();
  Args.Arg0 = 0x84000009;
  ArmCallSmc (&Args);
  CpuDeadLoop ();
}

VOID Mt6991WarmReset (VOID) {
  UINT32 Mode = MmioRead32 (DALI_WDT_BASE);
  Log ("DALI: warm RGU software reset for manual fastboot recovery\n");
  // Official mtk_wdt_restart sequence: select reset rather than IRQ and
  // issue SWRST. Preserve all other inherited MODE bits, including DRAM policy.
  MmioWrite32 (DALI_WDT_BASE, (Mode & ~BIT3) | 0x22000000);
  MemoryFence ();
  MmioWrite32 (DALI_WDT_BASE + 0x14, 0x1209);
  MemoryFence ();
  CpuDeadLoop ();
}

STATIC UINT64 Cells (CONST UINT32 *P, INT32 Count) {
  UINT64 Value = 0;
  for (INT32 I = 0; I < Count; I++) { Value = (Value << 32) | Fdt32ToCpu (P[I]); }
  return Value;
}

STATIC BOOLEAN OverlapsArena (UINT64 Base, UINT64 Size) {
  if (Size == 0) { return FALSE; }
  if (Base > MAX_UINT64 - Size) { return TRUE; }
  for (UINTN I = 0; I < 4; I++) {
    if (Base < mMap[I].Address + mMap[I].Length && mMap[I].Address < Base + Size) { return TRUE; }
  }
  return FALSE;
}

STATIC BOOLEAN CheckReservedNode (CONST VOID *Fdt, INT32 Node) {
  INT32 Length, Parent = FdtParentOffset (Fdt, Node);
  INT32 Ac = FdtAddressCells (Fdt, Parent), Sc = FdtSizeCells (Fdt, Parent);
  CONST UINT32 *Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Reg == NULL) { return TRUE; }
  if (Ac < 1 || Ac > 2 || Sc < 1 || Sc > 2 || Length <= 0 || Length % (4 * (Ac + Sc))) { return FALSE; }
  for (INT32 I = 0; I < Length / 4; I += Ac + Sc) {
    if (OverlapsArena (Cells (Reg + I, Ac), Cells (Reg + I + Ac, Sc))) { return FALSE; }
  }
  return TRUE;
}

STATIC EFI_STATUS SelectXnuMemory (CONST VOID *Fdt, UINTN BootFdt) {
  DALI_RAM_RANGE Fixed[ARRAY_SIZE (mMap)];
  UINTN FixedCount = 0, Exclusions = 0;
  UINT64 Base, Size;
  CHAR8 Message[160];
  // Everything currently loaded or accessed by UEFI remains outside the window.
  for (UINTN I = 0; I < ARRAY_SIZE (mMap) - 1; I++) {
    if (mMap[I].HobOption == AddMem &&
        !DaliAddRange (Fixed, &FixedCount, ARRAY_SIZE (Fixed), mMap[I].Address, mMap[I].Length)) {
      return EFI_COMPROMISED_DATA;
    }
  }
  if (!DaliWindowFromFdt (Fdt, BootFdt, Fixed, FixedCount, &Base, &Size, &Exclusions)) {
    Log ("DALI: no FDT-safe XNU memory window below 4 GiB\n");
    return EFI_OUT_OF_RESOURCES;
  }
  mMap[ARRAY_SIZE (mMap) - 1].Address = Base;
  mMap[ARRAY_SIZE (mMap) - 1].Length = Size;
  AsciiSPrint (Message, sizeof Message, "DALI: XNU RAM base=%lx size=%lx exclusions=%lu below-4G=1\n",
    Base, Size, (UINT64)Exclusions);
  Log (Message);
  return EFI_SUCCESS;
}

STATIC BOOLEAN CheckProbeRegion (CONST VOID *Fdt, CONST CHAR8 *Path, UINT64 Base) {
  CHAR8 Canonical[96], Message[160];
  UINTN I;
  // EDK2 PrintLib renders %x with A-F. DT unit addresses are lowercase.
  if (AsciiStrLen (Path) >= sizeof Canonical) { return FALSE; }
  for (I = 0; Path[I] != 0; I++) {
    Canonical[I] = (Path[I] >= 'A' && Path[I] <= 'F') ? Path[I] + ('a' - 'A') : Path[I];
  }
  Canonical[I] = 0;
  AsciiSPrint (Message, sizeof Message, "DALI: FDT probe begin %a", Canonical);
  LogTimed (Message);
  INT32 Length, Node = FdtPathOffset (Fdt, Canonical);
  if (Node < 0) {
    AsciiSPrint (Message, sizeof Message, "DALI: FDT probe region missing: %a\n", Canonical);
    Log (Message);
    return FALSE;
  }
  CONST UINT32 *Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Reg != NULL && Length >= 16 && Cells (Reg, 2) == Base && Cells (Reg + 2, 2) == 0x1000) {
    LogTimed ("DALI: FDT probe accepted");
    return TRUE;
  }
  AsciiSPrint (Message, sizeof Message, "DALI: FDT probe region rejected: %a\n", Canonical);
  Log (Message);
  return FALSE;
}

STATIC EFI_STATUS StopInheritedWatchdog (CONST VOID *Fdt) {
  CHAR8 Message[160];
  INT32 Node = FdtPathOffset (Fdt, "/watchdog@1c010000");
  UINT32 Before, After, Length;
  if (!CheckProbeRegion (Fdt, "/watchdog@1c010000", DALI_WDT_BASE) ||
      FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6991-wdt") < 0 ||
      FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6589-wdt") < 0) {
    return EFI_UNSUPPORTED;
  }
  // The mt6589-compatible RGU contract is documented by the official MT6991
  // Linux mtk_wdt driver: reload +0x08 with 0x1971, clear only MODE.EN with
  // the 0x22000000 write key. Do not touch reset requests or PMIC state.
  Before = MmioRead32 (DALI_WDT_BASE);
  Length = MmioRead32 (DALI_WDT_BASE + 4);
  if (Before == MAX_UINT32 || Length == MAX_UINT32) { return EFI_DEVICE_ERROR; }
  if (Before & BIT0) {
    MmioWrite32 (DALI_WDT_BASE + 8, 0x1971);
    MmioWrite32 (DALI_WDT_BASE, (Before & ~BIT0) | 0x22000000);
  }
  After = MmioRead32 (DALI_WDT_BASE);
  AsciiSPrint (Message, sizeof Message, "DALI: inherited RGU WDT mode=%x->%x length=%x disabled=%u\n",
    Before, After, Length, (After & BIT0) == 0);
  Log (Message);
  return (After & BIT0) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC BOOLEAN RestoreBootloaderMarker (UINT32 Original) {
  UINT32 Current = MmioRead32 (DALI_WDT_BASE + 0x24);
  if (Current == MAX_UINT32) { return FALSE; }
  MmioWrite32 (DALI_WDT_BASE + 0x24, (Current & ~0x0FU) | (Original & 0x0FU));
  MemoryFence ();
  Current = MmioRead32 (DALI_WDT_BASE + 0x24);
  return Current != MAX_UINT32 && (Current & 0x0FU) == (Original & 0x0FU);
}

STATIC EFI_STATUS PrepareFastboot (
  IN CONST VOID *Fdt,
  IN UINT32 TimeoutSeconds,
  IN CONST CHAR8 *RunToken,
  IN BOOLEAN ArmWatchdog
  ) {
  CHAR8 Message[224];
  INT32 Watchdog, RebootMode, Length;
  CONST UINT32 *Property;
  UINT32 OriginalMarker, Marker, Mode, Readback, WatchdogLength;
  if (Fdt == NULL || RunToken == NULL ||
      (ArmWatchdog && (TimeoutSeconds < 2 || TimeoutSeconds > 31))) {
    return EFI_INVALID_PARAMETER;
  }
  if (AsciiStrCmp (RunToken, "none") != 0) {
    if (AsciiStrLen (RunToken) != 32) { return EFI_INVALID_PARAMETER; }
    for (UINTN Index = 0; Index < 32; Index++) {
      if (!((RunToken[Index] >= '0' && RunToken[Index] <= '9') ||
            (RunToken[Index] >= 'a' && RunToken[Index] <= 'f'))) { return EFI_INVALID_PARAMETER; }
    }
  }
  Watchdog = FdtPathOffset (Fdt, "/watchdog@1c010000");
  RebootMode = FdtPathOffset (Fdt, "/watchdog@1c010000/reboot-mode");
  if (Watchdog < 0 || RebootMode < 0 ||
      !CheckProbeRegion (Fdt, "/watchdog@1c010000", DALI_WDT_BASE) ||
      FdtStringListSearch (Fdt, Watchdog, "compatible", "mediatek,mt6991-wdt") < 0 ||
      FdtStringListSearch (Fdt, Watchdog, "compatible", "mediatek,mt6589-wdt") < 0 ||
      FdtStringListSearch (Fdt, RebootMode, "compatible", "syscon-reboot-mode") < 0) {
    Log ("DALI: auto-fastboot FDT contract rejected\n");
    return EFI_UNSUPPORTED;
  }
  Property = FdtGetProp (Fdt, RebootMode, "offset", &Length);
  if (Property == NULL || Length != sizeof (UINT32) || Fdt32ToCpu (*Property) != 0x24) {
    return EFI_UNSUPPORTED;
  }
  Property = FdtGetProp (Fdt, RebootMode, "mask", &Length);
  if (Property == NULL || Length != sizeof (UINT32) || Fdt32ToCpu (*Property) != 0x0F) {
    return EFI_UNSUPPORTED;
  }
  Property = FdtGetProp (Fdt, RebootMode, "mode-bootloader", &Length);
  if (Property == NULL || Length != sizeof (UINT32) || Fdt32ToCpu (*Property) != 3) {
    return EFI_UNSUPPORTED;
  }

  OriginalMarker = MmioRead32 (DALI_WDT_BASE + 0x24);
  Mode = MmioRead32 (DALI_WDT_BASE);
  Readback = MmioRead32 (DALI_WDT_BASE + 4);
  if (OriginalMarker == MAX_UINT32 || Mode == MAX_UINT32 || Readback == MAX_UINT32 || (Mode & BIT0)) {
    Log ("DALI: auto-fastboot RGU state rejected\n");
    return EFI_DEVICE_ERROR;
  }

  Marker = (OriginalMarker & ~0x0FU) | 3;
  MmioWrite32 (DALI_WDT_BASE + 0x24, Marker);
  MemoryFence ();
  if ((MmioRead32 (DALI_WDT_BASE + 0x24) & 0x0FU) != 3) {
    BOOLEAN Restored = RestoreBootloaderMarker (OriginalMarker);
    AsciiSPrint (Message, sizeof Message,
      "DALI: auto-fastboot marker readback failed; rollback=%a\n", Restored ? "PASS" : "FAIL");
    Log (Message);
    return EFI_DEVICE_ERROR;
  }

  if (!ArmWatchdog) {
    Mode = MmioRead32 (DALI_WDT_BASE);
    if (Mode == MAX_UINT32 || (Mode & BIT0)) {
      BOOLEAN Restored = RestoreBootloaderMarker (OriginalMarker);
      AsciiSPrint (Message, sizeof Message,
        "DALI: reset qualification WDT state rejected mode=%x marker-rollback=%a\n",
        Mode, Restored ? "PASS" : "FAIL");
      Log (Message);
      return EFI_DEVICE_ERROR;
    }
    AsciiSPrint (Message, sizeof Message,
      "DALI: reset qualification run=%a marker=%x mode=%x watchdog=off\n",
      RunToken, Marker, Mode);
    Log (Message);
    return EFI_SUCCESS;
  }

  // Match Linux's seconds * 64 count units, shifted by five, with the key.
  // The driver caps a single interval at 31 seconds.
  WatchdogLength = (TimeoutSeconds << 11) | 0x08;
  MmioWrite32 (DALI_WDT_BASE + 4, WatchdogLength);
  MmioWrite32 (DALI_WDT_BASE + 8, 0x1971);
  MemoryFence ();
  Readback = MmioRead32 (DALI_WDT_BASE + 4);
  if (Readback == MAX_UINT32 || (Readback & 0xFFE0U) != (WatchdogLength & 0xFFE0U)) {
    BOOLEAN Restored = RestoreBootloaderMarker (OriginalMarker);
    AsciiSPrint (Message, sizeof Message,
      "DALI: auto-fastboot LENGTH rejected got=%x expected=%x marker-rollback=%a\n",
      Readback, WatchdogLength, Restored ? "PASS" : "FAIL");
    Log (Message);
    return EFI_DEVICE_ERROR;
  }
  Mode = MmioRead32 (DALI_WDT_BASE);
  if (Mode == MAX_UINT32) {
    RestoreBootloaderMarker (OriginalMarker);
    return EFI_DEVICE_ERROR;
  }
  MmioWrite32 (DALI_WDT_BASE, (Mode & ~(BIT3 | BIT6)) | BIT0 | 0x22000000);
  MemoryFence ();
  Readback = MmioRead32 (DALI_WDT_BASE);
  if (Readback == MAX_UINT32 || !(Readback & BIT0) || (Readback & (BIT3 | BIT6))) {
    BOOLEAN WatchdogDisabled = FALSE;
    if (Readback != MAX_UINT32) {
      MmioWrite32 (DALI_WDT_BASE, (Readback & ~BIT0) | 0x22000000);
      MemoryFence ();
      Mode = MmioRead32 (DALI_WDT_BASE);
      WatchdogDisabled = Mode != MAX_UINT32 && (Mode & BIT0) == 0;
    }
    BOOLEAN MarkerRestored = RestoreBootloaderMarker (OriginalMarker);
    AsciiSPrint (Message, sizeof Message,
      "DALI: auto-fastboot watchdog readback failed; disabled=%a marker-rollback=%a\n",
      WatchdogDisabled ? "PASS" : "FAIL", MarkerRestored ? "PASS" : "FAIL");
    Log (Message);
    return EFI_DEVICE_ERROR;
  }
  AsciiSPrint (Message, sizeof Message,
      "DALI: auto-fastboot armed run=%a marker=%x timeout=%u seconds; waiting for XNU to stop\n",
      RunToken, Marker, TimeoutSeconds);
  Log (Message);
  AsciiSPrint (Message, sizeof Message, "DALI: RGU arm readback mode=%x length=%x\n",
    Readback, MmioRead32 (DALI_WDT_BASE + 4));
  Log (Message);
  return EFI_SUCCESS;
}

EFI_STATUS Mt6991PrepareFastbootRecovery (
  IN CONST VOID *Fdt, IN UINT32 TimeoutSeconds, IN CONST CHAR8 *RunToken
  ) {
  return PrepareFastboot (Fdt, TimeoutSeconds, RunToken, TRUE);
}

EFI_STATUS Mt6991PrepareResetQualification (IN CONST VOID *Fdt, IN CONST CHAR8 *RunToken) {
  if (RunToken == NULL || AsciiStrLen (RunToken) != 32) { return EFI_INVALID_PARAMETER; }
  return PrepareFastboot (Fdt, 0, RunToken, FALSE);
}

EFI_STATUS Mt6991PlatformInitialize (IN UINTN BootFdt) {
  CONST VOID *Fdt = (VOID *)BootFdt;
  DALI_RAM_CONSOLE *Ring = (DALI_RAM_CONSOLE *)(UINTN)DALI_CONSOLE_BASE;
  INT32 Node, Depth, Length, Memory;
  CONST UINT32 *Reg;
  UINT64 Base, Size;
  UINT64 Sp, Sctlr, Cpacr, Vbar;
  CHAR8 Message[192];
  // Preserve a valid previous console. Fresh run tokens distinguish this boot.
  DaliConsoleInitialize (Ring, DALI_CONSOLE_SIZE - 12);
  Log ("\nDALI: SEC diagnostics v1\n");
  Mt6991LogRegisters ();
  Log ("DALI: SEC trace version=4\n");
  __asm__ volatile ("mov %0, sp" : "=r" (Sp));
  __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (Sctlr));
  __asm__ volatile ("mrs %0, cpacr_el1" : "=r" (Cpacr));
  __asm__ volatile ("mrs %0, vbar_el1" : "=r" (Vbar));
  AsciiSPrint (Message, sizeof Message, "DALI: SEC fdt=%lx sp=%lx sctlr=%lx cpacr=%lx vbar=%lx\n",
    (UINT64)BootFdt, Sp, Sctlr, Cpacr, Vbar);
  Log (Message);
  LogTimed ("DALI: SEC trace=header-check");
  if (BootFdt < 0x80000000ULL || BootFdt >= 0x100000000ULL || (BootFdt & 7) || FdtCheckHeader (Fdt)) {
    Log ("DALI: bad incoming FDT pointer/header\n");
    return EFI_INVALID_PARAMETER;
  }
  AsciiSPrint (Message, sizeof Message, "DALI: SEC FDT header bytes=%u\n", FdtTotalSize (Fdt));
  Log (Message);
  if (FdtTotalSize (Fdt) > DALI_FDT_SIZE || FdtTotalSize (Fdt) < sizeof (FDT_HEADER) ||
      FdtTotalSize (Fdt) > 0x100000000ULL - BootFdt ||
      OverlapsArena (BootFdt, FdtTotalSize (Fdt)) ||
      FdtStringListSearch (Fdt, 0, "compatible", "mediatek,MT6991") < 0) {
    Log ("DALI: SEC FDT extent/platform rejected\n");
    return EFI_UNSUPPORTED;
  }
  // Validate the watchdog's own FDT/MMIO contract before touching it, then
  // stop the inherited LK timer before the uncached, read-only FDT scans.
  // Recovery is armed separately by BDS after the payload opts in.
  LogTimed ("DALI: SEC trace=stop-inherited-watchdog");
  EFI_STATUS Status = StopInheritedWatchdog (Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  LogTimed ("DALI: SEC trace=memory-arenas");
  Memory = FdtPathOffset (Fdt, "/memory");
  Reg = FdtGetProp (Fdt, Memory, "reg", &Length);
  if (Memory < 0 || Reg == NULL || FdtAddressCells (Fdt, 0) != 2 || FdtSizeCells (Fdt, 0) != 2 ||
      Length <= 0 || Length % 16) { return EFI_UNSUPPORTED; }
  for (UINTN I = 0; I < 4; I++) {
    BOOLEAN Covered = FALSE;
    for (INT32 J = 0; J < Length / 4; J += 4) {
      Base = Cells (Reg + J, 2); Size = Cells (Reg + J + 2, 2);
      if (Base <= mMap[I].Address && Size <= MAX_UINT64 - Base &&
          mMap[I].Address + mMap[I].Length <= Base + Size) { Covered = TRUE; }
    }
    if (!Covered) { return EFI_OUT_OF_RESOURCES; }
  }
  LogTimed ("DALI: SEC trace=memreserve");
  for (INTN I = 0; I < FdtGetNumberOfReserveMapEntries (Fdt); I++) {
    if (FdtGetReserveMapEntry (Fdt, I, &Base, &Size) || OverlapsArena (Base, Size)) { return EFI_ACCESS_DENIED; }
  }
  LogTimed ("DALI: SEC trace=reserved-memory");
  Node = FdtPathOffset (Fdt, "/reserved-memory");
  if (Node < 0) { return EFI_UNSUPPORTED; }
  Depth = 0;
  for (INT32 Child = FdtNextNode (Fdt, Node, &Depth); Child >= 0 && Depth > 0; Child = FdtNextNode (Fdt, Child, &Depth)) {
    if (!CheckReservedNode (Fdt, Child)) { Log ("DALI: reserved-memory collision\n"); return EFI_ACCESS_DENIED; }
  }
  LogTimed ("DALI: SEC trace=protected-memory");
  Depth = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    if (FdtStringListSearch (Fdt, Node, "compatible", "pkvm,protected-region") >= 0 &&
        !CheckReservedNode (Fdt, Node)) { Log ("DALI: protected-region collision\n"); return EFI_ACCESS_DENIED; }
  }
  LogTimed ("DALI: SEC trace=initrd");
  Node = FdtPathOffset (Fdt, "/chosen");
  Reg = FdtGetProp (Fdt, Node, "linux,initrd-start", &Length);
  if (Reg != NULL && (Length == 4 || Length == 8)) {
    Base = Cells (Reg, Length / 4);
    Reg = FdtGetProp (Fdt, Node, "linux,initrd-end", &Length);
    if (Reg == NULL || (Length != 4 && Length != 8)) { return EFI_UNSUPPORTED; }
    Size = Cells (Reg, Length / 4);
    if (Size < Base || OverlapsArena (Base, Size - Base)) { return EFI_ACCESS_DENIED; }
  }
  LogTimed ("DALI: SEC trace=select-window");
  Status = SelectXnuMemory (Fdt, BootFdt);
  if (EFI_ERROR (Status)) { return Status; }
  LogTimed ("DALI: SEC trace=copy-fdt");
  CopyMem ((VOID *)(UINTN)DALI_FDT_BASE, Fdt, FdtTotalSize (Fdt));
  // Validate every MMIO address used by the optional display/key probes before
  // DXE can touch it. A changed firmware needs a new layout review.
  LogTimed ("DALI: SEC trace=mmio-contract");
  if (!CheckProbeRegion (Fdt, "/soc/pinctrl@1002d000", DALI_GPIO_BASE)) { return EFI_UNSUPPORTED; }
  for (UINTN I = 0; I < 3; I++) {
    CHAR8 Path[64]; UINT64 Address = 0x32490000 + I * 0x10000;
    AsciiSPrint (Path, sizeof Path, "/soc/dsi@%lx", Address);
    if (!CheckProbeRegion (Fdt, Path, Address)) { return EFI_UNSUPPORTED; }
  }
  for (UINTN Side = 0; Side < 2; Side++) {
    for (UINTN Unit = 0; Unit < 10; Unit++) {
      CHAR8 Path[80]; UINT64 Address = 0x32830000 + Side * 0x400000 + Unit * 0x10000;
      if (Unit == 1 || (Side == 1 && Unit == 0)) { continue; }
      AsciiSPrint (Path, sizeof Path, "/soc/disp-ovl%lu-exdma%lu@%lx", Side, Unit, Address);
      if (!CheckProbeRegion (Fdt, Path, Address)) { return EFI_UNSUPPORTED; }
    }
  }
  Log ("DALI: FDT copied; diagnostic RAM checked against carveouts\n");
  LogTimed ("DALI: SEC trace=platform-ready");
  return EFI_SUCCESS;
}
