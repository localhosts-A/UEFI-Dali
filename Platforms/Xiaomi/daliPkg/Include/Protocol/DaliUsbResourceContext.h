// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/DaliUsbPlatformLib.h>

extern EFI_GUID gDaliUsbResourceContextProtocolGuid;
typedef struct _DALI_USB_RESOURCE_CONTEXT_PROTOCOL DALI_USB_RESOURCE_CONTEXT_PROTOCOL;
typedef struct {
  EFI_STATUS InitializationStatus;
  UINT32 MappingAttempts;
  UINT32 MappedPages;
  BOOLEAN SnapshotValid;
  BOOLEAN BootServicesClosed;
} DALI_USB_RESOURCE_CONTEXT_STATUS;
/* BSP Boot Services interface. These are CPU device-memory mappings and an
 * immutable private FDT-derived description only: not a powered-source/rate
 * lease, hardware qualification, DMA map, HC registration or permission to
 * access unpowered MMIO. Output is copied; the private FDT is never exposed.
 * Mapping attempts (including partial failures) are retained for firmware
 * lifetime; no unload or removal of live CPU page-table mappings is supported.
 * GCD ownership and CPU attributes cover whole 4KiB pages, including the tail
 * half-page beyond APMIXED's logical window. Register access remains bounded
 * by each returned logical window; page rounding grants no extra MMIO access.
 * The controller worker must separately acquire all resource/clock/DMA/role
 * prerequisites. Every accessor refuses after the first EBS notification. */
struct _DALI_USB_RESOURCE_CONTEXT_PROTOCOL {
  UINT32 Revision;
  EFI_STATUS (EFIAPI *GetStatus)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_RESOURCE_CONTEXT_STATUS *);
  EFI_STATUS (EFIAPI *GetClockResources)(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *, DALI_USB_CLOCK_RESOURCES *);
};
