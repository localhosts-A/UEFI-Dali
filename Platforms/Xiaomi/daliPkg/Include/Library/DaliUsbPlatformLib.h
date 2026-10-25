// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <DaliUsbCalibration.h>
/* Read-only acquisition from a complete, immutable, 8-byte-aligned boot FDT
 * snapshot whose accessible allocation is FdtSize (at most16MiB). Out must be
 * distinct writable storage. Validate the Dali USB0/PHY/nvmem graph and decode
 * actual chosen ATAG payloads; no hardware accesses, allocation or retained
 * pointers. Check return status: output is committed only on complete success.
 * qualified describes calibration input only, not clock/DMA/power/hardware or
 * authenticity of the bootloader. The installed controller caller is separate. */
/* Requires the pinned BaseFdtLib implementation and its real fdt_check_full. */
EFI_STATUS DaliUsbReadCalibration (
  IN CONST VOID *Fdt, IN UINTN FdtSize,
  OUT struct dali_xsphy_calibration *Out
  );
