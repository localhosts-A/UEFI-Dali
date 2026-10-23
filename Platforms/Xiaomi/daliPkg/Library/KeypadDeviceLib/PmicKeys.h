// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
VOID DaliPmicKeysInitialize (CONST VOID *Fdt);
EFI_STATUS DaliPmicKeysPoll (UINT64 Now, UINT8 *Raw);
