// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/IoLib.h>
#include <Library/Mt6991PlatformLib.h>

// Deliberately expose only the validated diagnostic arena, not all 12 GiB.
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
  {"DSI state probe", 0x32490000, 0x30000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL0 state probe", 0x32830000, 0xA0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL1 state probe", 0x32C50000, 0x80000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"DISP0 mutex", 0x32020000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"DISP1 mutex", 0x32420000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL0 mutex", 0x32820000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"OVL1 mutex", 0x32C20000, 0x1000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
};

VOID Mt6991GetMemoryMap (OUT EFI_MEMORY_REGION_DESCRIPTOR **Map, OUT UINT8 *Count) {
  *Map = mMap;
  *Count = ARRAY_SIZE (mMap);
}

STATIC VOID Log (CONST CHAR8 *Text) {
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
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

STATIC BOOLEAN CheckProbeRegion (CONST VOID *Fdt, CONST CHAR8 *Path, UINT64 Base) {
  CHAR8 Canonical[96], Message[160];
  UINTN I;
  // EDK2 PrintLib renders %x with A-F. DT unit addresses are lowercase.
  if (AsciiStrLen (Path) >= sizeof Canonical) { return FALSE; }
  for (I = 0; Path[I] != 0; I++) {
    Canonical[I] = (Path[I] >= 'A' && Path[I] <= 'F') ? Path[I] + ('a' - 'A') : Path[I];
  }
  Canonical[I] = 0;
  INT32 Length, Node = FdtPathOffset (Fdt, Canonical);
  CONST UINT32 *Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Reg != NULL && Length >= 16 && Cells (Reg, 2) == Base && Cells (Reg + 2, 2) == 0x1000) { return TRUE; }
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

EFI_STATUS Mt6991PlatformInitialize (IN UINTN BootFdt) {
  CONST VOID *Fdt = (VOID *)BootFdt;
  volatile UINT32 *Ring = (volatile UINT32 *)(UINTN)DALI_CONSOLE_BASE;
  INT32 Node, Depth, Length, Memory;
  CONST UINT32 *Reg;
  UINT64 Base, Size;
  // Start a fresh console for this UEFI boot. All other ramoops zones stay intact.
  Ring[0] = 0x43474244; Ring[1] = 0; Ring[2] = 0;
  Log ("\nDALI: SEC diagnostics v1\n");
  Mt6991LogRegisters ();
  if (BootFdt < 0x80000000ULL || BootFdt >= 0x100000000ULL || (BootFdt & 7) || FdtCheckHeader (Fdt)) {
    Log ("DALI: bad incoming FDT pointer/header\n");
    return EFI_INVALID_PARAMETER;
  }
  if (FdtTotalSize (Fdt) > DALI_FDT_SIZE || FdtTotalSize (Fdt) < sizeof (FDT_HEADER) ||
      FdtTotalSize (Fdt) > 0x100000000ULL - BootFdt ||
      OverlapsArena (BootFdt, FdtTotalSize (Fdt)) ||
      FdtStringListSearch (Fdt, 0, "compatible", "mediatek,MT6991") < 0) {
    return EFI_UNSUPPORTED;
  }
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
  for (INTN I = 0; I < FdtGetNumberOfReserveMapEntries (Fdt); I++) {
    if (FdtGetReserveMapEntry (Fdt, I, &Base, &Size) || OverlapsArena (Base, Size)) { return EFI_ACCESS_DENIED; }
  }
  Node = FdtPathOffset (Fdt, "/reserved-memory");
  if (Node < 0) { return EFI_UNSUPPORTED; }
  for (INT32 Child = FdtFirstSubnode (Fdt, Node); Child >= 0; Child = FdtNextSubnode (Fdt, Child)) {
    if (!CheckReservedNode (Fdt, Child)) { Log ("DALI: reserved-memory collision\n"); return EFI_ACCESS_DENIED; }
  }
  Depth = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    if (FdtStringListSearch (Fdt, Node, "compatible", "pkvm,protected-region") >= 0 &&
        !CheckReservedNode (Fdt, Node)) { Log ("DALI: protected-region collision\n"); return EFI_ACCESS_DENIED; }
  }
  Node = FdtPathOffset (Fdt, "/chosen");
  Reg = FdtGetProp (Fdt, Node, "linux,initrd-start", &Length);
  if (Reg != NULL && (Length == 4 || Length == 8)) {
    Base = Cells (Reg, Length / 4);
    Reg = FdtGetProp (Fdt, Node, "linux,initrd-end", &Length);
    if (Reg == NULL || (Length != 4 && Length != 8)) { return EFI_UNSUPPORTED; }
    Size = Cells (Reg, Length / 4);
    if (Size < Base || OverlapsArena (Base, Size - Base)) { return EFI_ACCESS_DENIED; }
  }
  CopyMem ((VOID *)(UINTN)DALI_FDT_BASE, Fdt, FdtTotalSize (Fdt));
  // Validate every MMIO address used by the optional display/key probes before
  // DXE can touch it. A changed firmware needs a new layout review.
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
  return StopInheritedWatchdog (Fdt);
}
