// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/Mt6991UfsDmaLib.h>
#include <Protocol/Cpu.h>

// AArch64 CpuDxe maps WC to Normal Non-cacheable. UC is Device memory and
// cannot safely back the packed descriptors used by the generic UFS driver.
STATIC EFI_STATUS NonCachedAllocate (MT6991_DMA_REGION *R, UINTN Pages, UINT64 Limit, VOID *Context) {
  EFI_CPU_ARCH_PROTOCOL *Cpu = Context;
  EFI_PHYSICAL_ADDRESS Address = Limit;
  EFI_STATUS Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Address);
  if (EFI_ERROR (Status)) { return Status; }
  R->Live = TRUE; R->Cpu = (VOID *)(UINTN)Address; R->Device = Address; R->Pages = Pages;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  UINTN Bytes = EFI_PAGES_TO_SIZE (Pages);
  Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
  if (EFI_ERROR (Status)) { return Status; }
  if (Address < Descriptor.BaseAddress || Bytes > Descriptor.Length ||
      Address - Descriptor.BaseAddress > Descriptor.Length - Bytes ||
      !(Descriptor.Capabilities & EFI_MEMORY_WC) ||
      (Descriptor.Attributes & (EFI_MEMORY_RO | EFI_MEMORY_RP | EFI_MEMORY_WP))) { return EFI_UNSUPPORTED; }
  R->OriginalAttributes = Descriptor.Attributes;
  Status = Cpu->FlushDataCache (Cpu, Address, Bytes, EfiCpuFlushTypeWriteBackInvalidate);
  if (EFI_ERROR (Status)) { return Status; }
  UINT64 CacheMask = EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE;
  R->AttributesChanged = TRUE; // A failed update may still need restoration.
  Status = gDS->SetMemorySpaceAttributes (Address, Bytes, (Descriptor.Attributes & ~CacheMask) | EFI_MEMORY_WC);
  if (EFI_ERROR (Status)) { return Status; }
  return Cpu->FlushDataCache (Cpu, Address, Bytes, EfiCpuFlushTypeInvalidate);
}

STATIC EFI_STATUS NonCachedFree (MT6991_DMA_REGION *R, VOID *Context) {
  (VOID)Context;
  MemoryFence ();
  if (R->AttributesChanged && !R->AttributesRestored) {
    EFI_STATUS Status = gDS->SetMemorySpaceAttributes (R->Device, EFI_PAGES_TO_SIZE (R->Pages), R->OriginalAttributes);
    if (EFI_ERROR (Status)) { return Status; }
    R->AttributesRestored = TRUE;
  }
  return gBS->FreePages (R->Device, R->Pages);
}

STATIC VOID NonCachedFence (VOID *Context) { (VOID)Context; MemoryFence (); }

EFI_STATUS Mt6991UfsDmaUefiInit (MT6991_UFS_DMA *D, UINT64 Limit) {
  EFI_CPU_ARCH_PROTOCOL *Cpu;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) { return Status; }
  MT6991_DMA_OPS Ops = {NonCachedAllocate, NonCachedFree, NonCachedFence, Cpu};
  return Mt6991UfsDmaInit (D, &Ops, Limit);
}
