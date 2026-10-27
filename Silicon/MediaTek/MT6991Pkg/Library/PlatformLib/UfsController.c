// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/Mt6991UfsController.h>
#include "UfsProbe.h"

// Fixed MiCode device_modules e45c319: ufs-mediatek.c:242-347,391-455,
// 1686-1692,1931-2058; ufs-mediatek-sip.h; mtk_sip_svc.h. Fixed Linux
// f089d58: include/ufs/ufshci.h, unipro.h and ufshcd.c:4741-4793.
#define UFS_SIP 0xC2000276U
#define UFS_ERROR_MASK 0x70884U
#define UFS_UCCS 0x400U
#define UFS_UIC_ERROR 4U

STATIC BOOLEAN Bindings (CONST VOID *Fdt) {
  INT32 Length, Hci, Soc, Reset;
  CONST UINT32 *P;
  STATIC CONST UINT32 Range[] = {0, 0, 0, 0, 0x10, 0};
  STATIC CONST CHAR8 Names[] = "unipro_rst\0crypto_rst\0hci_rst";
  UINT32 Iommus;
  if (!DaliUfsResources (Fdt, &Iommus)) { return FALSE; }
  Soc = FdtPathOffset (Fdt, "/soc");
  Hci = FdtPathOffset (Fdt, "/soc/ufshci@16810000");
  Reset = FdtPathOffset (Fdt, "/syscon@168a0000/reset-controller");
  if (Reset < 0 || FdtParentOffset (Fdt, Reset) != FdtPathOffset (Fdt, "/syscon@168a0000") ||
      FdtStringListSearch (Fdt, Reset, "compatible", "ti,syscon-reset") < 0 ||
      FdtGetProp (Fdt, Hci, "iommus", &Length) != NULL) { return FALSE; }
  P = FdtGetProp (Fdt, Soc, "dma-ranges", &Length);
  if (P == NULL || Length != sizeof Range) { return FALSE; }
  for (UINTN I = 0; I < 6; I++) { if (Fdt32ToCpu (P[I]) != Range[I]) { return FALSE; } }
  INT32 Nodes[] = {0, Soc, Hci};
  for (UINTN I = 0; I < 3; I++) {
    if (FdtGetProp (Fdt, Nodes[I], "dma-coherent", &Length) != NULL ||
        FdtGetProp (Fdt, Nodes[I], "dma-noncoherent", &Length) != NULL) { return FALSE; }
  }
  if (FdtGetProp (Fdt, Hci, "mediatek,ufs-rtff-mtcmos", &Length) == NULL || Length != 0) { return FALSE; }
  // A board requiring the optional VA09 regulator sequence needs its own
  // verified power adapter. Never silently skip it based on PHY version alone.
  if (FdtGetProp (Fdt, Hci, "mediatek,ufs-support-va09", &Length) != NULL) { return FALSE; }
  CONST CHAR8 *N = FdtGetProp (Fdt, Hci, "reset-names", &Length);
  if (N == NULL || Length != sizeof Names || CompareMem (N, Names, sizeof Names)) { return FALSE; }
  P = FdtGetProp (Fdt, Reset, "#reset-cells", &Length);
  if (P == NULL || Length != 4 || Fdt32ToCpu (*P) != 1) { return FALSE; }
  P = FdtGetProp (Fdt, Hci, "resets", &Length);
  if (P == NULL || Length != 24) { return FALSE; }
  for (UINTN I = 0; I < 3; I++) {
    if (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (P[I * 2])) != Reset ||
        Fdt32ToCpu (P[I * 2 + 1]) != I + 1) { return FALSE; }
  }
  P = FdtGetProp (Fdt, Reset, "ti,reset-bits", &Length);
  if (P == NULL || Length != 112) { return FALSE; }
  for (UINTN I = 1; I <= 3; I++) {
    UINT32 Expected[] = {0x148, (UINT32)I - 1, 0x14C, (UINT32)I - 1, 0, 0, 0x1C};
    for (UINTN J = 0; J < 7; J++) { if (Fdt32ToCpu (P[I * 7 + J]) != Expected[J]) { return FALSE; } }
  }
  return TRUE;
}

STATIC VOID ReadUicSnapshot (MT6991_UFS_CONTROLLER *C, MT6991_UFS_UIC_SNAPSHOT *S) {
  C->Io.Fence (C->Io.Context);
  // Direct callback reads preserve LastAddress/Value and the original fault.
  S->InterruptStatus = C->Io.Read32 (DALI_UFS_HCI + 0x20, C->Io.Context);
  S->HostStatus = C->Io.Read32 (DALI_UFS_HCI + 0x30, C->Io.Context);
  S->HostEnable = C->Io.Read32 (DALI_UFS_HCI + 0x34, C->Io.Context);
  for (UINTN I = 0; I < 3; I++) { S->Arguments[I] = C->Io.Read32 (DALI_UFS_HCI + 0x94 + I * 4, C->Io.Context); }
  for (UINTN I = 0; I < 5; I++) { S->LayerErrors[I] = C->Io.Read32 (DALI_UFS_HCI + 0x38 + I * 4, C->Io.Context); }
  C->Io.Fence (C->Io.Context);
  S->Captured = TRUE;
}

STATIC BOOLEAN RecordFailure (MT6991_UFS_CONTROLLER *C, MT6991_UFS_RESULT Result, BOOLEAN CaptureUic) {
  if (!C->Failed) {
    C->Result = Result; C->FaultStage = C->Stage;
    C->FaultAddress = C->LastAddress; C->FaultValue = C->LastValue;
    // Only the real initialized instance may read these registers. In
    // particular, skip early power/reset failures and Stop's temporary context.
    if (CaptureUic && C->Attempted && C->Touched && C->Io.Read32 != NULL && !C->UicFailure.Captured &&
        (C->Stage == Mt6991UfsPreLink || C->Stage == Mt6991UfsLinkStartup || C->Stage == Mt6991UfsPostLink)) {
      ReadUicSnapshot (C, &C->UicFailure);
    }
  }
  C->Failed = TRUE; C->Ready = FALSE;
  return FALSE;
}

STATIC BOOLEAN Fail (MT6991_UFS_CONTROLLER *C, MT6991_UFS_RESULT Result) {
  return RecordFailure (C, Result, TRUE);
}

STATIC UINT32 Read (MT6991_UFS_CONTROLLER *C, UINTN Address) {
  C->LastAddress = Address; C->LastValue = C->Io.Read32 (Address, C->Io.Context);
  return C->LastValue;
}

STATIC BOOLEAN Write (MT6991_UFS_CONTROLLER *C, UINTN Address, UINT32 Value) {
  C->LastAddress = Address; C->LastValue = Value; C->Touched = TRUE;
  if (!C->Io.Write32 (Address, Value, C->Io.Context)) { return Fail (C, Mt6991UfsHardwareError); }
  C->Io.Fence (C->Io.Context);
  return TRUE;
}

STATIC BOOLEAN Wait (MT6991_UFS_CONTROLLER *C, UINT32 Offset, UINT32 Mask,
                     UINT32 Expected, UINT32 TimeoutUs) {
  // One final read at the deadline; never an unbounded status poll.
  for (UINT32 Elapsed = 0; ; Elapsed += 10) {
    UINT32 Value = Read (C, DALI_UFS_HCI + Offset);
    if (Value == 0xFFFFFFFFU) { return Fail (C, Mt6991UfsHardwareError); }
    if ((Value & Mask) == Expected) { return TRUE; }
    if (Elapsed >= TimeoutUs) { return Fail (C, Mt6991UfsTimeout); }
    C->Io.Stall (10, C->Io.Context);
  }
}

STATIC BOOLEAN Sip (MT6991_UFS_CONTROLLER *C, UINT32 Selector, UINT32 A, UINT32 B) {
  C->Touched = TRUE;
  C->SipSelector = Selector; C->SipArg1 = A; C->SipArg2 = B;
  C->SipStatus = C->Io.Sip (UFS_SIP, Selector, A, B, C->Io.Context);
  return C->SipStatus == 0 || Fail (C, Mt6991UfsSipError);
}

STATIC BOOLEAN DeviceReset (MT6991_UFS_CONTROLLER *C) {
  // Fixed Linux ufshcd_init resets the device before its first HBA enable.
  // MTK ufs_mtk_device_reset: selector BIT(1), active-low pulse, HBA stop in
  // the low interval, 10..15us low and 10..15ms after release.
  C->DeviceResetAsserted = Sip (C, 2, 0, 0);
  BOOLEAN Stopped = C->DeviceResetAsserted && Write (C, DALI_UFS_HCI + 0x34, 0) &&
    Wait (C, 0x34, 1, 0, 100000);
  C->Io.Stall (10, C->Io.Context);
  C->DeviceResetReleaseAttempted = TRUE;
  if (!Stopped) {
    // One first deassertion for cleanup, even if assertion/HBA stop failed.
    // Preserve the original failure/SipStatus; do not retry a failed release.
    C->DeviceResetCleanupAttempted = TRUE;
    C->DeviceResetCleanupStatus = C->Io.Sip (UFS_SIP, 2, 1, 0, C->Io.Context);
    C->DeviceResetReleased = C->DeviceResetCleanupStatus == 0;
    if (C->DeviceResetReleased) { C->Io.Stall (15000, C->Io.Context); }
    return FALSE;
  }
  if (!Sip (C, 2, 1, 0)) { return FALSE; }
  C->DeviceResetReleased = TRUE;
  C->Io.Stall (15000, C->Io.Context);
  return TRUE;
}

STATIC BOOLEAN Rmw (MT6991_UFS_CONTROLLER *C, UINT32 Offset, UINT32 Mask, UINT32 Value) {
  UINT32 Old = Read (C, DALI_UFS_HCI + Offset);
  if (Old == 0xFFFFFFFFU) { return Fail (C, Mt6991UfsHardwareError); }
  return Write (C, DALI_UFS_HCI + Offset, (Old & ~Mask) | Value);
}

STATIC BOOLEAN ResetBit (MT6991_UFS_CONTROLLER *C, UINT32 Offset, UINT32 Mask) {
  // reset-ti-syscon.c:84,115 uses regmap_write_bits (forced masked RMW).
  // ASSERT_SET/DEASSERT_SET describe bit polarity, not W1S register semantics.
  UINT32 Old = Read (C, DALI_UFS_AO + Offset);
  if (Old == 0xFFFFFFFFU) { return Fail (C, Mt6991UfsHardwareError); }
  return Write (C, DALI_UFS_AO + Offset, Old | Mask);
}

STATIC BOOLEAN AckUic (MT6991_UFS_CONTROLLER *C, UINT32 Mask) {
  if (!Write (C, DALI_UFS_HCI + 0x20, Mask)) { return FALSE; }
  C->InterruptStatus = Read (C, DALI_UFS_HCI + 0x20);
  // Cover status arriving between the final completion poll and this W1C,
  // including unrelated bits outside the generic transport error mask.
  if ((C->Stage == Mt6991UfsLinkStartup || (Mask & UFS_UIC_ERROR)) &&
      C->InterruptStatus != 0) { return Fail (C, Mt6991UfsHardwareError); }
  return !(C->InterruptStatus & (UFS_ERROR_MASK | UFS_UCCS)) || Fail (C, Mt6991UfsHardwareError);
}

STATIC BOOLEAN EmptyQueues (MT6991_UFS_CONTROLLER *C) {
  STATIC CONST UINT32 Offsets[] = {0x50, 0x54, 0x58, 0x60, 0x70, 0x74, 0x78, 0x80, 0x300};
  for (UINTN I = 0; I < sizeof Offsets / sizeof Offsets[0]; I++) {
    UINT32 Value = Read (C, DALI_UFS_HCI + Offsets[I]);
    if (Value == 0xFFFFFFFFU) { return Fail (C, Mt6991UfsHardwareError); }
    if (Value != 0) { return Fail (C, Mt6991UfsBusy); }
  }
  return TRUE;
}

STATIC BOOLEAN LinkResetEvent (MT6991_UFS_CONTROLLER *C) {
  // Linux enables UCCS, not UIC_ERROR, during startup (ufshcd.c:4788-4789).
  // The ISR acknowledges the full IS when completion arrives (:6948-6955),
  // and UECPA is cleared after POST_CHANGE (:4957-4958). This serial adapter
  // classifies all layers at real completion, before any post-link command.
  // Never clear/read-clear the indication while LINK_STARTUP is in progress.
  if (C->Stage != Mt6991UfsLinkStartup || C->UicOpcode != 0x16 || C->LinkResetEvent.Captured ||
      C->InterruptStatus != (UFS_UIC_ERROR | UFS_UCCS)) { return Fail (C, Mt6991UfsHardwareError); }
  MT6991_UFS_UIC_SNAPSHOT *S = &C->LinkResetEvent;
  ReadUicSnapshot (C, S);
  BOOLEAN Isolated = S->InterruptStatus == (UFS_UIC_ERROR | UFS_UCCS) &&
    S->HostEnable == 1 && S->HostStatus != 0xFFFFFFFFU && (S->HostStatus & 0x38) == 8 &&
    S->LayerErrors[0] == 0x80000010U;
  for (UINTN I = 1; I < 5; I++) { Isolated = Isolated && S->LayerErrors[I] == 0; }
  if (!Isolated) {
    // Preserve the first read-clear codes; Fail must not resample them as zero.
    C->UicFailure = *S;
    return Fail (C, Mt6991UfsHardwareError);
  }
  C->LinkResetAccepted = TRUE;
  return TRUE;
}

STATIC BOOLEAN Uic (MT6991_UFS_CONTROLLER *C, UINT32 Opcode, UINT32 Attribute, UINT32 *Value) {
  if (C->Failed || !Wait (C, 0x30, 8, 8, 500000)) { return FALSE; }
  if (C->LastValue & 0x30) { return Fail (C, Mt6991UfsHardwareError); }
  C->InterruptStatus = Read (C, DALI_UFS_HCI + 0x20);
  if (C->InterruptStatus & UFS_ERROR_MASK) { return Fail (C, Mt6991UfsHardwareError); }
  C->UicOpcode = Opcode; C->UicResult = 0xFFFFFFFFU;
  // Clear ONLY an old command-completion bit (IS is W1C), then order arguments
  // before the command. IE stays zero: completion is polled without IRQs.
  if (!AckUic (C, UFS_UCCS) ||
      !Write (C, DALI_UFS_HCI + 0x94, Attribute << 16) ||
      !Write (C, DALI_UFS_HCI + 0x98, 0) ||
      !Write (C, DALI_UFS_HCI + 0x9C, *Value) ||
      !Write (C, DALI_UFS_HCI + 0x90, Opcode)) { return FALSE; }
  BOOLEAN LinkStartup = C->Stage == Mt6991UfsLinkStartup && Opcode == 0x16;
  for (UINT32 Elapsed = 0; ; Elapsed += 10) {
    C->InterruptStatus = Read (C, DALI_UFS_HCI + 0x20);
    if (LinkStartup) {
      // A latched UIC_ERROR is not a new event on each poll. Fatal/reserved or
      // unrelated bits still fail immediately, without extending the deadline.
      if (C->InterruptStatus & ~(UFS_UIC_ERROR | UFS_UCCS)) { return Fail (C, Mt6991UfsHardwareError); }
      if (C->InterruptStatus & UFS_UIC_ERROR) {
        if (!C->LinkResetPending) { C->LinkResetFirstIs = C->InterruptStatus; }
        C->LinkResetPending = TRUE; ++C->LinkResetPolls;
      }
    } else if (C->InterruptStatus & UFS_ERROR_MASK) { return Fail (C, Mt6991UfsHardwareError); }
    if (C->InterruptStatus & UFS_UCCS) { break; }
    if (Elapsed >= 500000) { return Fail (C, Mt6991UfsTimeout); }
    C->Io.Stall (10, C->Io.Context);
  }
  if (LinkStartup && C->LinkResetPending && !LinkResetEvent (C)) { return FALSE; }
  UINT32 Result = Read (C, DALI_UFS_HCI + 0x98);
  C->UicResult = Result & 0xFF;
  if (Result == 0xFFFFFFFFU) { return Fail (C, Mt6991UfsHardwareError); }
  if (C->UicResult != 0) { return Fail (C, Mt6991UfsUicError); }
  *Value = Read (C, DALI_UFS_HCI + 0x9C);
  if (*Value == 0xFFFFFFFFU) { return Fail (C, Mt6991UfsHardwareError); }
  // Only after actual UCCS, isolated PA classification and a successful result
  // acknowledge the latched indication together with completion. No early W1C4.
  UINT32 Ack = UFS_UCCS | ((LinkStartup && C->LinkResetPending) ? UFS_UIC_ERROR : 0);
  if (!AckUic (C, Ack)) { return FALSE; }
  if (Ack & UFS_UIC_ERROR) { C->LinkResetAcked = TRUE; }
  return TRUE;
}

STATIC BOOLEAN DmeSet (MT6991_UFS_CONTROLLER *C, UINT32 Attribute, UINT32 Value) {
  return Uic (C, 2, Attribute, &Value);
}

STATIC BOOLEAN DmeRmw (MT6991_UFS_CONTROLLER *C, UINT32 Attribute, UINT32 Mask, UINT32 Bits) {
  UINT32 Value = 0;
  return Uic (C, 1, Attribute, &Value) && DmeSet (C, Attribute, (Value & ~Mask) | Bits);
}

MT6991_UFS_RESULT Mt6991UfsStop (MT6991_UFS_CONTROLLER *C) {
  if (C == NULL) { return Mt6991UfsRejected; }
  C->Ready = FALSE;
  if (C->Stopped || !C->Touched) {
    C->Stopped = TRUE; C->StopResult = Mt6991UfsStopped; return C->StopResult;
  }
  MT6991_UFS_CONTROLLER Q;
  ZeroMem (&Q, sizeof Q); Q.Io = C->Io; Q.Stage = C->Stage;
  // Do not release buffers on a stop failure. The future mapping owner may
  // release them only after this routine confirms HCE disabled.
  if (!Write (&Q, DALI_UFS_HCI + 0x24, 0) ||
      !Write (&Q, DALI_UFS_HCI + 0x60, 0) || !Write (&Q, DALI_UFS_HCI + 0x80, 0) ||
      !Write (&Q, DALI_UFS_HCI + 0x34, 0) || !Wait (&Q, 0x34, 1, 0, 100000)) {
    C->StopResult = Q.Result;
    if (!C->Failed) {
      C->LastAddress = Q.FaultAddress; C->LastValue = Q.FaultValue;
      // Queue/HCE writes have already started. Keep the stop fault without
      // misrepresenting a post-stop sample as the first UIC failure snapshot.
      RecordFailure (C, Q.Result, FALSE);
    }
    return C->StopResult;
  }
  C->Stopped = TRUE; C->StopResult = Mt6991UfsStopped;
  return C->StopResult;
}

MT6991_UFS_RESULT Mt6991UfsStart (CONST VOID *Fdt, BOOLEAN Enabled, CONST CHAR8 *Token,
                               CONST MT6991_UFS_IO *Io, MT6991_UFS_CONTROLLER *C) {
  DALI_UFS_SNAPSHOT S;
  if (C == NULL) { return Mt6991UfsRejected; }
  if (!Enabled) { return Mt6991UfsOff; }
  if (C->Stopped) { return C->Failed ? C->Result : Mt6991UfsRejected; }
  if (C->Attempted) { return C->Ready ? Mt6991UfsLinkReady : C->Failed ? C->Result : Mt6991UfsRejected; }
  C->Attempted = TRUE;
  if (Io == NULL || Io->Read32 == NULL || Io->Write32 == NULL || Io->Fence == NULL ||
      Io->Stall == NULL || Io->Sip == NULL || !DaliUfsToken (Token) || !Bindings (Fdt)) {
    Fail (C, Mt6991UfsRejected); return C->Result;
  }
  C->Io = *Io;
  if (DaliUfsSnapshot (Fdt, TRUE, Token, Io->Read32, Io->Context, &S) != DaliUfsProbeCaptured ||
      S.MtkIpVersion != 0x10470000U || S.Version != 0x400 || S.Cap != 0x55C7051FU ||
      S.IommusBytes || (S.Gate0 & 3) || (S.Gate1 & 0x11F)) {
    Fail (C, Mt6991UfsRejected); return C->Result;
  }
  if (S.Hce || S.Hcs || S.UtrlLo || S.UtrlHi || S.UtrlDoorbell || S.UtrlRun ||
      S.UtmrlLo || S.UtmrlHi || S.UtmrlDoorbell || S.UtmrlRun || S.MemConfig || S.Ie) {
    Fail (C, Mt6991UfsBusy); return C->Result;
  }
  C->Stage = Mt6991UfsPower;
  // Version-1 PHY power_on is a no-op. Preserve rails/parent clocks; never
  // execute the old version-0 analog recipe, global bypass or crypto/RPMB SMC.
  if (!Sip (C, 1U << 9, 0, 0) || !Sip (C, 1U << 9, 1, 0)) { goto Failed; }
  // Secure power restoration must not silently reactivate an old request list.
  if (!Wait (C, 0x34, 1, 0, 0) || !EmptyQueues (C)) { goto Failed; }
  C->Stage = Mt6991UfsReferenceClock;
  UINT32 RefClock = Read (C, DALI_UFS_HCI + 0x144);
  if (RefClock == 0xFFFFFFFFU) { Fail (C, Mt6991UfsHardwareError); goto Failed; }
  if ((RefClock & 3) != 3) {
    if (!Sip (C, 1U << 3, 1, 0)) { goto Failed; }
    if (!Write (C, DALI_UFS_HCI + 0x144, 1) || !Wait (C, 0x144, 3, 3, 3000)) {
      // Same timeout notification as the fixed MTK source: mark clock off at
      // POST_CHANGE, without replaying the failed request or losing its fault.
      C->RefClockCleanupStatus = C->Io.Sip (UFS_SIP, 1U << 3, 0, 1, C->Io.Context);
      goto Failed;
    }
    C->Io.Stall (32, C->Io.Context);
    if (!Sip (C, 1U << 3, 1, 1)) { goto Failed; }
  }
  C->Stage = Mt6991UfsDeviceReset;
  if (!DeviceReset (C)) { goto Failed; }
  C->Stage = Mt6991UfsHostReset;
  // Live ti,syscon-reset IDs 3/2/1 -> bits 2/1/0, reversed on release.
  if (!ResetBit (C, 0x148, 4) || !ResetBit (C, 0x148, 2) ||
      !ResetBit (C, 0x148, 1)) { goto Failed; }
  C->Io.Stall (100, C->Io.Context);
  if (!ResetBit (C, 0x14C, 1) || !ResetBit (C, 0x14C, 2) ||
      !ResetBit (C, 0x14C, 4) ||
      !Write (C, DALI_UFS_HCI + 0x18, 0) || !Write (C, DALI_UFS_HCI + 0x24, 0) ||
      !Rmw (C, 0x140, 0, 0x80) ||
      !Rmw (C, 0x160, (0x7FFFU << 8) | (1U << 28), 0x453000U | (1U << 25))) { goto Failed; }
  C->Stage = Mt6991UfsHce;
  if (!Write (C, DALI_UFS_HCI + 0x34, 1)) { goto Failed; }
  C->Io.Stall (600, C->Io.Context);
  if (!Wait (C, 0x34, 1, 1, 50000) || !EmptyQueues (C)) { goto Failed; }
  C->Stage = Mt6991UfsPreLink;
  C->LocalVersion = 0;
  if (!Uic (C, 1, 0x15A9, &C->LocalVersion) || !DmeSet (C, 0xD0A8, 0) ||
      !DmeSet (C, 0x155E, 0) || !DmeRmw (C, 0xD0A6, 1U << 6, 0)) { goto Failed; }
  C->Stage = Mt6991UfsLinkStartup;
  UINT32 Value = 0;
  if (!Uic (C, 0x16, 0, &Value) || !Wait (C, 0x30, 1, 1, 50000)) { goto Failed; }
  C->Stage = Mt6991UfsPostLink;
  if (!DmeRmw (C, 0xD09E, 0, 0x100) || !DmeRmw (C, 0xD0A6, 0, 0xD) ||
      !DmeRmw (C, 0xD0A1, 1U << 5, 0)) { goto Failed; }
  C->Ready = TRUE; C->Result = Mt6991UfsLinkReady;
  return C->Result;
Failed:
  Mt6991UfsStop (C);
  return C->Result;
}
