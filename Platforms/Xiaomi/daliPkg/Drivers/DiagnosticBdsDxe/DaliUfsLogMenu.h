// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
EFI_STATUS DaliUfsLogMenu (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *In,
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out, CONST UINT8 *RecordToken);
