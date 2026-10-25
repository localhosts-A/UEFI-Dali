// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>

typedef struct {
  VOID *Cpu;
  EFI_PHYSICAL_ADDRESS Address;
  UINTN Size;
  VOID *Cookie;
  BOOLEAN Coherent;
} DALI_USB_XHCI_DMA_BUFFER;
typedef enum { DaliXhciToDevice, DaliXhciFromDevice, DaliXhciBidirectional } DALI_USB_XHCI_DMA_DIRECTION;
typedef enum { DaliXhciValidate, DaliXhciRelease } DALI_USB_XHCI_IO_PHASE;
typedef struct {
  VOID *Context;
  BOOLEAN Qualified;
  EFI_STATUS (EFIAPI *Allocate)(VOID *, UINTN, UINTN, UINT64, DALI_USB_XHCI_DMA_BUFFER *);
  EFI_STATUS (EFIAPI *Sync)(VOID *, DALI_USB_XHCI_DMA_BUFFER *, UINTN, UINTN, DALI_USB_XHCI_DMA_DIRECTION, BOOLEAN);
  EFI_STATUS (EFIAPI *Free)(VOID *, DALI_USB_XHCI_DMA_BUFFER *);
} DALI_USB_XHCI_DMA_OPS;
typedef EFI_STATUS (EFIAPI *DALI_USB_XHCI_IO_GUARD)(VOID *, DALI_USB_XHCI_IO_PHASE);
typedef struct {
  // Private compatibility ABI for pinned Mu register/schedule/memory code.
  // NEVER install this as EFI_PCI_IO_PROTOCOL: no PCI device is represented.
  EFI_PCI_IO_PROTOCOL Io;
  UINTN Base, Size, OpOffset;
  DALI_USB_XHCI_DMA_OPS Dma;
  DALI_USB_XHCI_IO_GUARD Guard;
  VOID *GuardContext, *Records;
  UINTN RecordCount, AllocatedBytes;
  BOOLEAN Closed, Initialized, Owned, Faulted, Quiescing, HaltProven;
} DALI_USB_XHCI_IO;

/* Serialized persistent noncopyable context, initially zeroed. The owner sets
 * Closed permanently at its independently registered EBS event; calls cannot
 * overlap EBS or run on an AP. All callbacks may run at TPL_NOTIFY: provider and
 * proof callbacks must be bounded/nonblocking and must not reenter.
 * Guard proves powered device CPU mappings, held source/domain/PHY/host role,
 * connector/VBUS and exclusive controller/IRQ ownership plus real device DMA
 * translation/cache policy held for the ENTIRE lifetime, including failures.
 * Qualified is a provider obligation, never inferred from a low CPU address.
 * Allocate supplies a mapped private contiguous buffer, requested-aligned CPU
 * AND device addresses, exactly Size bytes and limit<=0xffffffff. Common
 * buffers request64KiB alignment to preserve Mu's CPU-based ring boundary and
 * <=64KiB aligned-page calculations in the device address space. The consumer
 * must select a supported HC page size<=64KiB. On partial error
 * it MUST report any retained ownership in Buffer. Free is retryable; success
 * completes unmap/cache ownership and release, failure retains Buffer intact.
 * Common buffers must be uncached/coherent during concurrent CPU/device use.
 * Sync and Free may not modify the descriptor, including on failure.
 * No mapping provider or hardware qualification is supplied by this library.
 * Mu's completion/cancellation proof authorizes normal Unmap/Free. Host data
 * must remain valid until successful Unmap or discarded
 * consumer teardown; Close never copies back retained data. An outstanding
 * streaming alias also prevents FreeBuffer of this adapter's own CPU pages.
 * Prefix/suffix FreeBuffer is logical trimming: the full underlying allocation
 * stays reserved/accounted until its remaining interval is freed. This private
 * ABI deliberately trades early padding reclamation for indivisible DMA holds.
 * Every error
 * latches new activity off; its independent records survive ignored Mu cleanup
 * errors. Quiesce stops new activity and proves a fresh hardware halt. Close
 * additionally requires Release proof that ALL source HC/queue/users have been
 * destroyed; it discards retained data without dereferencing expired callers.
 * Release is proof only, not a place to destroy users or drop held resources. */
EFI_STATUS DaliUsbXhciIoOpen(DALI_USB_XHCI_IO *, UINTN Base, UINTN Size,
  CONST DALI_USB_XHCI_DMA_OPS *, DALI_USB_XHCI_IO_GUARD, VOID *);
EFI_STATUS DaliUsbXhciIoQuiesce(DALI_USB_XHCI_IO *, UINT32 TimeoutUs);
EFI_STATUS DaliUsbXhciIoClose(DALI_USB_XHCI_IO *);
