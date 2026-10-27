// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/Mt6991PlatformLib.h>
#include "UfsProbe.h"

STATIC VOID UfsLog (CONST CHAR8 *Message) {
  SerialPortWrite ((UINT8 *)Message, AsciiStrLen (Message));
}

STATIC UINT32 UfsRead32 (UINTN Address, VOID *Context) {
  CHAR8 Line[128];
  AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a read32=%lx\n", (CONST CHAR8 *)Context, (UINT64)Address);
  UfsLog (Line);
  return MmioRead32 (Address);
}

EFI_STATUS Mt6991ProbeUfs (CONST VOID *Fdt) {
  DALI_UFS_SNAPSHOT Snapshot;
  CONST CHAR8 *Token = FixedPcdGetPtr (PcdDaliUfsProbeToken);
  CHAR8 Line[256];
  if (!FixedPcdGetBool (PcdDaliUfsProbeEnabled)) { return EFI_NOT_STARTED; }
  if (!DaliUfsToken (Token)) { return EFI_INVALID_PARAMETER; }
  AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a begin version=2 mode=read-only writes=0\n", Token);
  UfsLog (Line);
  DALI_UFS_PROBE_RESULT Result = DaliUfsSnapshot (Fdt, TRUE, Token, UfsRead32, (VOID *)Token, &Snapshot);
  AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a gates=%08x,%08x iommu-property-bytes=%u result=%u\n",
    Token, Snapshot.Gate0, Snapshot.Gate1, Snapshot.IommusBytes, Result);
  UfsLog (Line);
  if (Snapshot.HciRead) {
    AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a cap=%08x ver=%08x hce=%08x hcs=%08x is=%08x ie=%08x ahit=%08x\n",
      Token, Snapshot.Cap, Snapshot.Version, Snapshot.Hce, Snapshot.Hcs, Snapshot.Is, Snapshot.Ie, Snapshot.Ahit);
    UfsLog (Line);
    AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a utrl=%08x%08x db=%08x rs=%08x utmrl=%08x%08x db=%08x rs=%08x\n",
      Token, Snapshot.UtrlHi, Snapshot.UtrlLo, Snapshot.UtrlDoorbell, Snapshot.UtrlRun,
      Snapshot.UtmrlHi, Snapshot.UtmrlLo, Snapshot.UtmrlDoorbell, Snapshot.UtmrlRun);
    UfsLog (Line);
    AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a mcq-read=%u mcqcap=%08x memcfg=%08x mcqcfg=%08x\n",
      Token, Snapshot.McqRead, Snapshot.McqCap, Snapshot.MemConfig, Snapshot.McqConfig);
    UfsLog (Line);
    AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a mtk-ip=%08x refclk=%08x xoufs=%08x opt=%08x otsd=%08x mphy-ver=1\n",
      Token, Snapshot.MtkIpVersion, Snapshot.RefClock, Snapshot.XoUfs, Snapshot.Options, Snapshot.Outstanding);
    UfsLog (Line);
  }
  AsciiSPrint (Line, sizeof Line, "DALI-UFS: run=%a end result=%u dma-qualified=0 media-qualified=0\n", Token, Result);
  UfsLog (Line);
  return Result == DaliUfsProbeCaptured ? EFI_SUCCESS : EFI_NOT_READY;
}
