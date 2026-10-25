// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DaliPmicBus.h>

STATIC volatile UINT32 mBusy;
STATIC BOOLEAN mClosing;
STATIC DALI_PMIC_BUS_TOKEN *mOwner;
STATIC DALI_PMIC_BUS_TOKEN *mReserved;
STATIC DALI_PMIC_BUS_PROTOCOL mProtocol;
STATIC EFI_EVENT mExitEvent;
STATIC EFI_HANDLE mHandle;

STATIC EFI_STATUS EFIAPI TryAcquire (
  DALI_PMIC_BUS_PROTOCOL *This, DALI_PMIC_BUS_TOKEN *Token)
{
  if (This != &mProtocol || Token == NULL) { return EFI_INVALID_PARAMETER; }
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (InterlockedCompareExchange32 (&mBusy, 0, 1) != 0) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  if (mClosing || Token->Active || (mReserved != NULL && mReserved != Token)) {
    InterlockedCompareExchange32 (&mBusy, 1, 0);
    gBS->RestoreTPL (OldTpl);
    return Token->Active ? EFI_INVALID_PARAMETER : EFI_NOT_READY;
  }
  Token->OldTpl = OldTpl;
  Token->Active = TRUE;
  mOwner = Token;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Release (
  DALI_PMIC_BUS_PROTOCOL *This, DALI_PMIC_BUS_TOKEN *Token, BOOLEAN PendingOwned)
{
  if (This != &mProtocol || Token == NULL) { return EFI_INVALID_PARAMETER; }
  if (mOwner != Token || !Token->Active) { return EFI_ACCESS_DENIED; }
  EFI_TPL OldTpl = Token->OldTpl;
  Token->Active = FALSE;
  mReserved = PendingOwned ? Token : NULL;
  mOwner = NULL;
  InterlockedCompareExchange32 (&mBusy, 1, 0);
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC DALI_PMIC_BUS_PROTOCOL mProtocol = {
  DALI_PMIC_BUS_REVISION, TryAcquire, Release
};

STATIC VOID EFIAPI ExitBootServices (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  mClosing = TRUE;
}

EFI_STATUS EFIAPI DaliPmicBusInitialize (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  DALI_PMIC_BUS_PROTOCOL *Existing;
  (VOID)ImageHandle; (VOID)SystemTable;
  EFI_STATUS Status = gBS->LocateProtocol (&gDaliPmicBusProtocolGuid, NULL, (VOID **)&Existing);
  if (!EFI_ERROR (Status)) { return EFI_ALREADY_STARTED; }
  if (Status != EFI_NOT_FOUND) { return Status; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, NULL,
    &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle, &gDaliPmicBusProtocolGuid, &mProtocol, NULL);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mExitEvent); mExitEvent = NULL; }
  return Status;
}
