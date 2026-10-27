// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DaliUfsLog.h>
#include "DaliUfsLogMenu.h"

EFI_STATUS DaliUfsLogMenu (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *In,
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out, CONST UINT8 *RecordToken, UINT64 Generation) {
  if (In == NULL || In->ReadKeyStroke == NULL || Out == NULL || Out->OutputString == NULL) { return EFI_UNSUPPORTED; }
  DALI_UFS_LOG_PROTOCOL *Log = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&gDaliUfsLogProtocolGuid, NULL, (VOID **)&Log);
  if (Status != EFI_SUCCESS) { return Status; }
  if (Log == NULL || Log->Revision != DALI_UFS_LOG_REVISION || Log->Run == NULL) { return EFI_UNSUPPORTED; }
  UINT32 Mode = DALI_UFS_LOG_READBACK;
  Status = Out->OutputString (Out, L"\r\nUEFI userdata log diagnostic\r\nUp: select WRITE  Down: select READBACK\r\nPower: confirm; no input for 30s: skip\r\nSelected: READBACK (no writes)\r\n");
  if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
  for (UINTN Tick = 0; Tick < 3000; ++Tick) {
    EFI_INPUT_KEY Key;
    if (In->ReadKeyStroke (In, &Key) == EFI_SUCCESS) {
      if (Key.ScanCode == SCAN_UP || Key.ScanCode == SCAN_DOWN) {
        Mode = Key.ScanCode == SCAN_UP ? DALI_UFS_LOG_WRITE : DALI_UFS_LOG_READBACK;
        Status = Out->OutputString (Out, Mode == DALI_UFS_LOG_WRITE ? L"Selected: WRITE new userdata log\r\n" : L"Selected: READBACK (no writes)\r\n");
        if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; }
      }
      else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN || Key.ScanCode == 0x0102) {
        return Log->Run (Log, Mode, RecordToken, Mode == DALI_UFS_LOG_WRITE ? 0 : Generation);
      }
    }
    gBS->Stall (10000);
  }
  Out->OutputString (Out, L"UFS log diagnostic skipped; no log write submitted.\r\n");
  return EFI_ABORTED;
}
