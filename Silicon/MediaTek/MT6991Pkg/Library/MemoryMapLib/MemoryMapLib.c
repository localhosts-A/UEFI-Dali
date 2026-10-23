// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/Mt6991PlatformLib.h>
VOID GetMemoryMap (OUT EFI_MEMORY_REGION_DESCRIPTOR **Map, OUT UINT8 *Count) {
  Mt6991GetMemoryMap (Map, Count);
}
