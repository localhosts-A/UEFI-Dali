// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DALI_PMIC_BUS_PROTOCOL_H
#define DALI_PMIC_BUS_PROTOCOL_H
#include <Uefi.h>

#define DALI_PMIC_BUS_REVISION 1U
extern EFI_GUID gDaliPmicBusProtocolGuid;
typedef struct _DALI_PMIC_BUS_PROTOCOL DALI_PMIC_BUS_PROTOCOL;

// Each client keeps a persistent, zero-initialized token. Never copy or free a
// live/reserved token. The DXE singleton identifies ownership by its address.
typedef struct {
  EFI_TPL OldTpl;
  BOOLEAN Active;
} DALI_PMIC_BUS_TOKEN;

typedef EFI_STATUS (EFIAPI *DALI_PMIC_BUS_TRY_ACQUIRE)(
  IN DALI_PMIC_BUS_PROTOCOL *This, IN OUT DALI_PMIC_BUS_TOKEN *Token);
typedef EFI_STATUS (EFIAPI *DALI_PMIC_BUS_RELEASE)(
  IN DALI_PMIC_BUS_PROTOCOL *This, IN OUT DALI_PMIC_BUS_TOKEN *Token,
  IN BOOLEAN PendingOwned);

struct _DALI_PMIC_BUS_PROTOCOL {
  UINT32 Revision;
  DALI_PMIC_BUS_TRY_ACQUIRE TryAcquire;
  DALI_PMIC_BUS_RELEASE Release;
};

// All Dali SWINF2 users (SID4 keypad and SID14 USB) use this same DXE instance.
// TryAcquire never waits: EFI_NOT_READY means contention or EBS shutdown, and
// leaves the token unchanged. Success holds TPL_HIGH_LEVEL until Release, so
// transactions must use bounded MMIO/counter delays only: no allocation,
// protocol lookup, logging, events or other Boot Services while holding it.
// Boot Services callers run on the BSP. Release requires the original token
// and caller context; an invalid release cannot unlock another owner.
// This protocol only serializes software. It never reads/writes PMIF, adopts
// foreign transactions, initializes clocks/routes or authorizes PMIC writes.
// Release(PendingOwned=TRUE) reserves the channel for this client across lease
// boundaries, while restoring TPL. Other tokens cannot acquire until this same
// client recovers its pending command and releases with PendingOwned=FALSE.
// This prevents a delayed owner from consuming a different client's response,
// even if the original command later leaves the hardware channel idle.
// It rejects new leases after the first ExitBootServices notification, including
// EBS retries; USB quiesce must already have completed before the final map.
#endif
