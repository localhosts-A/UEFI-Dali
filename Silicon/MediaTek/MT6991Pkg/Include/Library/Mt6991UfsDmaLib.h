// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define MT6991_UFS_DMA_SLOTS 16
#define MT6991_UFS_DMA_MAX_TRANSFER 0x100000U
typedef enum { Mt6991DmaDeviceRead, Mt6991DmaDeviceWrite, Mt6991DmaCommon } MT6991_DMA_OPERATION;

typedef struct {
  VOID *Cpu;
  UINT64 Device, OriginalAttributes;
  UINTN Pages, References;
  BOOLEAN Live, Common, Quarantined, AttributesChanged, AttributesRestored;
} MT6991_DMA_REGION;

// Allocate must record Live/Cpu/Device/Pages immediately after obtaining pages,
// including on later failures. Free deletes nothing until restoration AND page
// release succeed. No IOMMU/SID/global bypass is part of this interface.
typedef struct {
  EFI_STATUS (*Allocate)(MT6991_DMA_REGION *, UINTN Pages, UINT64 Limit, VOID *);
  EFI_STATUS (*Free)(MT6991_DMA_REGION *, VOID *);
  VOID (*Fence)(VOID *);
  VOID *Context;
} MT6991_DMA_OPS;

typedef struct {
  UINT64 Id;
  UINTN Region, Offset, Length;
  VOID *Host;
  MT6991_DMA_OPERATION Operation;
  BOOLEAN Live, Submitted, CopyBack;
} MT6991_DMA_MAP;

typedef struct {
  MT6991_DMA_OPS Ops;
  MT6991_DMA_REGION Regions[MT6991_UFS_DMA_SLOTS];
  MT6991_DMA_MAP Maps[MT6991_UFS_DMA_SLOTS];
  UINT64 Limit, NextId;
  BOOLEAN Initialized, Busy, Poisoned, Stopped;
} MT6991_UFS_DMA;

EFI_STATUS Mt6991UfsDmaInit (MT6991_UFS_DMA *, CONST MT6991_DMA_OPS *, UINT64 Limit);
EFI_STATUS Mt6991UfsDmaUefiInit (MT6991_UFS_DMA *, UINT64 Limit);
EFI_STATUS Mt6991UfsDmaAllocate (MT6991_UFS_DMA *, UINTN Pages, VOID **Host);
EFI_STATUS Mt6991UfsDmaFree (MT6991_UFS_DMA *, UINTN Pages, VOID *Host);
EFI_STATUS Mt6991UfsDmaMap (MT6991_UFS_DMA *, MT6991_DMA_OPERATION, VOID *Host,
                           UINTN Bytes, UINT64 *Device, UINT64 *MappingId);
EFI_STATUS Mt6991UfsDmaUnmap (MT6991_UFS_DMA *, UINT64 MappingId);
// Resolve only an active owned mapping; used to validate descriptors before
// ringing a hardware doorbell. Every byte must lie within one owned mapping.
EFI_STATUS Mt6991UfsDmaResolve (MT6991_UFS_DMA *, UINT64 Device, UINTN Bytes,
                               BOOLEAN RequireCommon, VOID **Cpu);
EFI_STATUS Mt6991UfsDmaSubmit (MT6991_UFS_DMA *);
// Caller must have observed the doorbell clear and validated completion.
EFI_STATUS Mt6991UfsDmaComplete (MT6991_UFS_DMA *, BOOLEAN DataValid);
VOID Mt6991UfsDmaPoison (MT6991_UFS_DMA *);
// After poisoning, pages stay quarantined until the caller closes submissions,
// stops lists and observes HCE=0. This function never resets the poison flag.
EFI_STATUS Mt6991UfsDmaStopped (MT6991_UFS_DMA *, UINT32 ObservedHce);
EFI_STATUS Mt6991UfsDmaReleaseAll (MT6991_UFS_DMA *);
