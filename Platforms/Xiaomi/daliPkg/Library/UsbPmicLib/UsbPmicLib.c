// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbPmicLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_STATUS UsbStatus (int Result) {
  switch (Result) {
    case USB_OK: return EFI_SUCCESS;
    case USB_BUSY: return EFI_NOT_READY;
    case USB_TIMEOUT: return EFI_TIMEOUT;
    case USB_UNSUPPORTED: return EFI_UNSUPPORTED;
    case USB_INVALID: case USB_RANGE: return EFI_INVALID_PARAMETER;
    default: return EFI_DEVICE_ERROR;
  }
}
STATIC int BusAcquire (VOID *Opaque) {
  DALI_USB_PMIC *Context = Opaque;
  if (Context->Faulted) { return USB_IO; }
  EFI_STATUS Status = Context->Bus->TryAcquire (Context->Bus, &Context->Token);
  return Status == EFI_NOT_READY ? USB_BUSY : EFI_ERROR (Status) ? USB_IO : USB_OK;
}
STATIC int BusRelease (VOID *Opaque, bool PendingOwned) {
  DALI_USB_PMIC *Context = Opaque;
  EFI_STATUS Status = Context->Bus->Release (Context->Bus, &Context->Token, PendingOwned);
  if (EFI_ERROR (Status)) { Context->Faulted = TRUE; return USB_IO; }
  return USB_OK;
}
STATIC int Read32 (VOID *Opaque, uint32_t Offset, uint32_t *Value) {
  DALI_USB_PMIC *Context = Opaque;
  if (!Context->Token.Active || Context->Faulted) { return USB_IO; }
  if ((Offset & 3) || Offset > 0x8fb || Value == NULL) { return USB_RANGE; }
  *Value = MmioRead32 (DALI_PMIF_BASE + Offset);
  return USB_OK;
}
STATIC int Write32 (VOID *Opaque, uint32_t Offset, uint32_t Value) {
  DALI_USB_PMIC *Context = Opaque;
  if (!Context->Token.Active || Context->Faulted) { return USB_IO; }
  if ((Offset & 3) || Offset > 0x8fb) { return USB_RANGE; }
  MmioWrite32 (DALI_PMIF_BASE + Offset, Value);
  return USB_OK;
}
STATIC VOID Barrier (VOID *Opaque) { (VOID)Opaque; MemoryFence (); }
STATIC VOID Delay (VOID *Opaque, uint32_t Microseconds) {
  (VOID)Opaque; MicroSecondDelay (Microseconds);
}
STATIC BOOLEAN Enabled (CONST VOID *Fdt, INT32 Node) {
  INT32 Length;
  CONST CHAR8 *Status = FdtGetProp (Fdt, Node, "status", &Length);
  if (Status == NULL) { return TRUE; }
  return (Length == 3 && Status[0] == 'o' && Status[1] == 'k' && Status[2] == 0) ||
    (Length == 5 && Status[0] == 'o' && Status[1] == 'k' && Status[2] == 'a' && Status[3] == 'y' && Status[4] == 0);
}
STATIC BOOLEAN Cell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 Expected) {
  INT32 Length;
  CONST UINT32 *Value = FdtGetProp (Fdt, Node, Name, &Length);
  return Value != NULL && Length == 4 && Fdt32ToCpu (*Value) == Expected;
}
STATIC BOOLEAN ValidateFdt (CONST VOID *Fdt, UINT16 *Mask) {
  CONST UINT32 Expected[] = {0, 0x1c01a000, 0, 0x8ff, 0, 0x1c01c000, 0, 0x100,
                            0, 0x1c018000, 0, 0x8ff, 0, 0x1c01c800, 0, 0x100};
  INT32 Length;
  if (Fdt == NULL || FdtCheckHeader (Fdt)) { return FALSE; }
  INT32 Root = FdtPathOffset (Fdt, "/");
  if (Root < 0 || !Enabled (Fdt, Root) || !Cell (Fdt, Root, "#address-cells", 2) || !Cell (Fdt, Root, "#size-cells", 2)) { return FALSE; }
  INT32 Node = FdtPathOffset (Fdt, "/spmi@1c01a000");
  if (Node < 0 || !Enabled (Fdt, Node) || !Cell (Fdt, Node, "#address-cells", 2) || !Cell (Fdt, Node, "#size-cells", 0) ||
      FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6991-spmi") < 0) { return FALSE; }
  CONST CHAR8 *Names[] = {"pmif", "spmimst", "pmif-p", "spmimst-p"};
  for (UINTN I = 0; I < ARRAY_SIZE (Names); ++I) {
    if (FdtStringListSearch (Fdt, Node, "reg-names", Names[I]) != (INT32)I) { return FALSE; }
  }
  CONST UINT32 *Values = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Values == NULL || Length != sizeof Expected) { return FALSE; }
  for (UINTN I = 0; I < ARRAY_SIZE (Expected); ++I) {
    if (Fdt32ToCpu (Values[I]) != Expected[I]) { return FALSE; }
  }
  Values = FdtGetProp (Fdt, Node, "spmi-dev-mask", &Length);
  if (Values == NULL || Length != 4) { return FALSE; }
  UINT32 Route = Fdt32ToCpu (Values[0]);
  if (Route > 0xffff || (Route & BIT14)) { return FALSE; }
  Node = FdtPathOffset (Fdt, "/spmi@1c01a000/mt6379@e");
  if (Node < 0 || !Enabled (Fdt, Node) || FdtStringListSearch (Fdt, Node, "compatible", "mediatek,mt6379") < 0) { return FALSE; }
  Values = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Values == NULL || Length != 8 || Fdt32ToCpu (Values[0]) != 14 || Values[1] != 0) { return FALSE; }
  *Mask = (UINT16)Route; return TRUE;
}
EFI_STATUS DaliUsbPmicOpen (DALI_USB_PMIC *Context, CONST VOID *Fdt) {
  if (Context == NULL) { return EFI_INVALID_PARAMETER; }
  if (Context->Bus != NULL || Context->Transport.initialized || Context->Token.Active || Context->Faulted) {
    return EFI_ALREADY_STARTED;
  }
  UINT16 Mask;
  if (!ValidateFdt (Fdt, &Mask)) { return EFI_UNSUPPORTED; }
  EFI_STATUS Status = gBS->LocateProtocol (&gDaliPmicBusProtocolGuid, NULL, (VOID **)&Context->Bus);
  if (EFI_ERROR (Status)) { Context->Bus = NULL; return Status; }
  if (Context->Bus->Revision != DALI_PMIC_BUS_REVISION || Context->Bus->TryAcquire == NULL || Context->Bus->Release == NULL) {
    Context->Bus = NULL; return EFI_UNSUPPORTED;
  }
  struct usb_mmio Mmio = {Context, 0x8ff, Read32, Write32, Barrier, Delay};
  struct dali_pmif_lock Lock = {Context, BusAcquire, BusRelease};
  int Result = dali_usb_pmif_init (&Context->Transport, &Mmio, &Lock, Mask, true);
  if (!Result) { Result = dali_mt6379_identify (&Context->Transport); }
  if (!Result) { Context->Ready = TRUE; }
  else if (!Context->Transport.initialized && !Context->Token.Active && !Context->Faulted) {
    ZeroMem (Context, sizeof *Context);
  }
  return UsbStatus (Result);
}
EFI_STATUS DaliUsbPmicClose (DALI_USB_PMIC *Context) {
  if (Context == NULL) { return EFI_INVALID_PARAMETER; }
  if (Context->Faulted || Context->Token.Active) { return EFI_DEVICE_ERROR; }
  if (Context->Transport.initialized && (Context->Transport.faulted || Context->Transport.pending_owned)) {
    int Result = dali_usb_pmif_recover (&Context->Transport);
    if (Result) { return UsbStatus (Result); }
  }
  ZeroMem (Context, sizeof *Context);
  return EFI_SUCCESS;
}
