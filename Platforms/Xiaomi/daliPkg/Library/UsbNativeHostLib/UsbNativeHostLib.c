// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbNativeHostLib.h>
#include <Library/BaseLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.h"

extern EFI_USB2_HC_PROTOCOL gXhciUsb2HcTemplate;
BOOLEAN XhcCheckUrbResult(USB_XHCI_INSTANCE *, URB *);
EFI_STATUS XhcFlushAsyncIntMap(USB_XHCI_INSTANCE *, URB *);
typedef struct {
  USB_XHCI_INSTANCE Raw;
  DALI_USB_NATIVE_HOST *Owner;
  UINT32 LastShortEndpointMask[256];
} ENGINE;
#define RAW(H) (&((ENGINE *)(H)->Engine)->Raw)
STATIC DALI_USB_NATIVE_HOST *Host(EFI_USB2_HC_PROTOCOL *P) {
  return BASE_CR(P,DALI_USB_NATIVE_HOST,Protocol);
}
STATIC EFI_TPL CurrentTpl(VOID) {
  EFI_TPL T=gBS->RaiseTPL(TPL_HIGH_LEVEL); gBS->RestoreTPL(T); return T;
}
STATIC EFI_STATUS Fault(DALI_USB_NATIVE_HOST *H, EFI_STATUS Status) {
  H->Faulted=TRUE; H->Accepting=FALSE;
  if (H->Io!=NULL && !H->Io->Closed && H->Io->Owned) {
    H->Io->Faulted=TRUE;
    // Failure retains source and DMA ownership. This never destroys users.
    DaliUsbXhciIoQuiesce(H->Io,1000000);
  }
  return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
}
EFI_STATUS DaliNativeXhciCheck(USB_XHCI_INSTANCE *X) {
  DALI_USB_NATIVE_HOST *H=BASE_CR(X,ENGINE,Raw)->Owner;
  if (H->Closed || H->Io->Closed) { return EFI_UNSUPPORTED; }
  if (H->Faulted || H->Io->Faulted || H->Io->Quiescing || H->Prepared) { return EFI_DEVICE_ERROR; }
  return H->Io->Io.Flush(&H->Io->Io);
}
VOID DaliNativeXhciCommandFailed(USB_XHCI_INSTANCE *X, EFI_STATUS Status) {
  Fault(BASE_CR(X,ENGINE,Raw)->Owner,Status);
}
VOID DaliNativeXhciRecordShortCompletion(USB_XHCI_INSTANCE *X, UINT8 SlotId,
                                          UINT8 EndpointId, BOOLEAN Short) {
  if (X==NULL || SlotId==0 || EndpointId==0 || EndpointId>31) { return; }
  ENGINE *E=BASE_CR(X,ENGINE,Raw);
  DALI_USB_NATIVE_HOST *H=E->Owner;
  USB_DEV_CONTEXT *D=&X->UsbDevContext[SlotId];
  if (H==NULL || H->Engine!=E || H->Closed || !D->Enabled || D->SlotId!=SlotId) { return; }
  UINT32 Mask=(UINT32)1U<<EndpointId;
  if (Short) { E->LastShortEndpointMask[SlotId]|=Mask; }
  else { E->LastShortEndpointMask[SlotId]&=~Mask; }
}
BOOLEAN DaliNativeXhciIgnoreSpuriousSuccess(USB_XHCI_INSTANCE *X, UINT8 SlotId,
                                             UINT8 EndpointId, UINT8 CompletionCode) {
  if (X==NULL || CompletionCode!=TRB_COMPLETION_SUCCESS || SlotId==0 ||
      EndpointId==0 || EndpointId>31) { return FALSE; }
  ENGINE *E=BASE_CR(X,ENGINE,Raw);
  DALI_USB_NATIVE_HOST *H=E->Owner;
  USB_DEV_CONTEXT *D=&X->UsbDevContext[SlotId];
  UINT32 Mask=(UINT32)1U<<EndpointId;
  if (H==NULL || H->Engine!=E || H->Closed || !D->Enabled || D->SlotId!=SlotId ||
      !(E->LastShortEndpointMask[SlotId]&Mask)) { return FALSE; }
  E->LastShortEndpointMask[SlotId]&=~Mask;
  return TRUE;
}
VOID DaliNativeXhciClearShortCompletions(USB_XHCI_INSTANCE *X, UINT8 SlotId) {
  if (X==NULL || SlotId==0) { return; }
  ENGINE *E=BASE_CR(X,ENGINE,Raw);
  DALI_USB_NATIVE_HOST *H=E->Owner;
  if (H!=NULL && H->Engine==E && !H->Closed) { E->LastShortEndpointMask[SlotId]=0; }
}
STATIC EFI_STATUS Enter(DALI_USB_NATIVE_HOST *H, BOOLEAN Cleanup, EFI_TPL *Old) {
  if (H==NULL || CurrentTpl()>TPL_NOTIFY) { return EFI_UNSUPPORTED; }
  *Old=gBS->RaiseTPL(TPL_NOTIFY);
  if (InterlockedCompareExchange32(&H->Busy,0,1)!=0) { gBS->RestoreTPL(*Old); return EFI_NOT_READY; }
  EFI_STATUS S=EFI_SUCCESS;
  if (H->Closed || H->Io==NULL || H->Io->Closed) { S=EFI_UNSUPPORTED; }
  else if (H->Engine==NULL) { S=EFI_NOT_STARTED; }
  else if (!Cleanup && (H->Faulted || H->Io->Faulted || !H->Accepting || H->Prepared)) { S=EFI_DEVICE_ERROR; }
  if (EFI_ERROR(S)) { H->Busy=0; gBS->RestoreTPL(*Old); }
  return S;
}
STATIC EFI_STATUS Leave(DALI_USB_NATIVE_HOST *H, EFI_TPL Old, EFI_STATUS Status) {
  if (H->Closed || H->Io->Closed) { Status=EFI_UNSUPPORTED; }
  else if (H->Io->Faulted) { Status=Fault(H,EFI_DEVICE_ERROR); }
  // A partially initialized slot must never become available to another call.
  else if (!H->Prepared && H->Engine!=NULL) {
    for (UINTN I=1; I<256; I++) {
      USB_DEV_CONTEXT *D=&RAW(H)->UsbDevContext[I];
      if (D->Enabled && (D->SlotId!=I || D->InputContext==NULL || D->OutputContext==NULL ||
          D->EndpointTransferRing[0]==NULL || ((TRANSFER_RING *)D->EndpointTransferRing[0])->RingSeg0==NULL)) {
        Status=Fault(H,EFI_DEVICE_ERROR); break;
      }
    }
    if (Status==EFI_TIMEOUT || Status==EFI_OUT_OF_RESOURCES) { Status=Fault(H,Status); }
  }
  H->Epoch++; H->Busy=0; gBS->RestoreTPL(Old); return Status;
}
STATIC EFI_STATUS Read32(DALI_USB_NATIVE_HOST *H, UINTN Offset, UINT32 *Value) {
  return H->Io->Io.Mem.Read(&H->Io->Io,EfiPciIoWidthUint32,0,Offset,1,Value);
}
STATIC BOOLEAN Fits(DALI_USB_NATIVE_HOST *H, UINTN Offset, UINTN Bytes) {
  return Offset<=H->Io->Size && Bytes<=H->Io->Size-Offset;
}
STATIC EFI_STATUS Capabilities(DALI_USB_NATIVE_HOST *H) {
  USB_XHCI_INSTANCE *X=RAW(H);
  UINT32 V=0,Hcs=0,Hcc=0,Db=0,Rt=0,Pages=0;
  EFI_STATUS S=Read32(H,0,&V);
  if (!EFI_ERROR(S)) { S=Read32(H,4,&Hcs); }
  if (!EFI_ERROR(S)) { S=Read32(H,8,&X->HcSParams2.Dword); }
  if (!EFI_ERROR(S)) { S=Read32(H,16,&Hcc); }
  if (!EFI_ERROR(S)) { S=Read32(H,20,&Db); }
  if (!EFI_ERROR(S)) { S=Read32(H,24,&Rt); }
  if (EFI_ERROR(S)) { return S; }
  X->CapLength=(UINT8)V; X->HcSParams1.Dword=Hcs; X->HcCParams.Dword=Hcc;
  X->DBOff=Db&~3U; X->RTSOff=Rt&~31U; X->ExtCapRegBase=(Hcc>>16)*4;
  X->UsbLegSupOffset=X->DebugCapSupOffset=X->Usb2SupOffset=X->Usb3SupOffset=0xffffffff;
  X->PageSize=4096; X->MaxInterrupt=(UINT16)X->HcSParams1.Data.MaxIntrs;
  UINTN N=X->HcSParams1.Data.MaxPorts;
  if ((V>>16)<0x100 || (V>>16)>0x120 || X->CapLength!=H->Io->OpOffset ||
      !X->HcSParams1.Data.MaxSlots || !N || !X->MaxInterrupt ||
      !Fits(H,X->CapLength,0x400+N*16) || !Fits(H,X->DBOff,((UINTN)X->HcSParams1.Data.MaxSlots+1)*4) ||
      !Fits(H,X->RTSOff,0x20+(UINTN)X->MaxInterrupt*32) ||
      X->DBOff<X->CapLength+0x400+N*16 || X->RTSOff<X->CapLength+0x400+N*16 ||
      (X->DBOff<X->RTSOff ? X->RTSOff-X->DBOff<((UINTN)X->HcSParams1.Data.MaxSlots+1)*4 :
       X->DBOff-X->RTSOff<0x20+(UINTN)X->MaxInterrupt*32)) { return EFI_UNSUPPORTED; }
  S=Read32(H,X->CapLength+8,&Pages);
  if (EFI_ERROR(S) || !(Pages&1)) { return EFI_ERROR(S) ? S : EFI_UNSUPPORTED; }
  UINTN Offset=X->ExtCapRegBase; BOOLEAN Ports[256]={FALSE};
  for (UINTN Count=0; Offset!=0; Count++) {
    if (Count>=256 || Offset<0x20 || !Fits(H,Offset,4)) { return EFI_UNSUPPORTED; }
    UINT32 Header=0,A=0,B=0; S=Read32(H,Offset,&Header); if (EFI_ERROR(S)) { return S; }
    UINTN Id=Header&255, Next=((Header>>8)&255)*4, Minimum=4;
    if (Id==1) {
      Minimum=8;
      if (!Fits(H,Offset,Minimum) || (Header&0x01010000)) { return EFI_ACCESS_DENIED; }
      S=Read32(H,Offset+4,&A);
      if (EFI_ERROR(S) || (A&0xffff)) { return EFI_ERROR(S) ? S : EFI_ACCESS_DENIED; }
    } else if (Id==10) {
      Minimum=0x24;
      if (!Fits(H,Offset,Minimum)) { return EFI_UNSUPPORTED; }
      S=Read32(H,Offset+0x20,&A);
      if (EFI_ERROR(S) || (A&1)) { return EFI_ERROR(S) ? S : EFI_ACCESS_DENIED; }
    } else if (Id==2) {
      Minimum=16;
      if (!Fits(H,Offset,Minimum)) { return EFI_UNSUPPORTED; }
      S=Read32(H,Offset+4,&A);
      if (!EFI_ERROR(S)) { S=Read32(H,Offset+8,&B); }
      if (EFI_ERROR(S)) { return S; }
      if (A!=0x20425355 || !(B&255) || !((B>>8)&255) || (B&255)+((B>>8)&255)-1>N) { return EFI_UNSUPPORTED; }
      Minimum+=((B>>28)&15)*4;
      if (!Fits(H,Offset,Minimum)) { return EFI_UNSUPPORTED; }
      if ((Header>>24)==2) {
        // Raw source uses one USB2 Supported Protocol window; custom PSI
        // requires an explicit speed-map implementation outside this profile.
        if (X->Usb2SupOffset!=0xffffffff || (B>>28)!=0) { return EFI_UNSUPPORTED; }
        X->Usb2SupOffset=(UINT32)(Offset-X->ExtCapRegBase);
        for (UINTN I=(B&255)-1; I<(B&255)-1+((B>>8)&255); I++) { Ports[I]=TRUE; }
      }
    }
    if (Next && (Next<Minimum || !Fits(H,Offset,Next+4))) { return EFI_UNSUPPORTED; }
    Offset=Next ? Offset+Next : 0;
  }
  for (UINTN I=0; I<H->PortCount; I++) {
    if (H->Ports[I]>=N || !Ports[H->Ports[I]]) { return EFI_UNSUPPORTED; }
    for (UINTN J=0; J<I; J++) { if (H->Ports[J]==H->Ports[I]) { return EFI_INVALID_PARAMETER; } }
  }
  return EFI_SUCCESS;
}
STATIC VOID FreeUsers(DALI_USB_NATIVE_HOST *H) {
  USB_XHCI_INSTANCE *X=RAW(H);
  while (!IsListEmpty(&X->AsyncIntTransfers)) {
    URB *U=BASE_CR(GetFirstNode(&X->AsyncIntTransfers),URB,UrbList);
    RemoveEntryList(&U->UrbList); VOID *Data=U->Data; XhcFreeUrb(X,U);
    if (Data!=NULL) { FreePool(Data); }
  }
  for (UINTN I=1; I<256; I++) {
    USB_DEV_CONTEXT *D=&X->UsbDevContext[I];
    for (UINTN J=0; J<31; J++) { if (D->EndpointTransferRing[J]!=NULL) { FreePool(D->EndpointTransferRing[J]); } }
    if (D->ConfDesc!=NULL) {
      for (UINTN J=0; J<D->DevDesc.NumConfigurations; J++) { if (D->ConfDesc[J]!=NULL) { FreePool(D->ConfDesc[J]); } }
      FreePool(D->ConfDesc);
    }
    if (D->ActiveAlternateSetting!=NULL) { FreePool(D->ActiveAlternateSetting); }
    ZeroMem(D,sizeof *D);
  }
  ZeroMem(((ENGINE *)H->Engine)->LastShortEndpointMask,
          sizeof ((ENGINE *)H->Engine)->LastShortEndpointMask);
  // Context/ring DMA allocations belong to MemPool, freed exactly once below.
  XhcFreeSched(X);
}
STATIC EFI_STATUS Initialize(DALI_USB_NATIVE_HOST *H, BOOLEAN Run) {
  USB_XHCI_INSTANCE *X=RAW(H);
  EFI_STATUS S=XhcResetHC(X,XHC_RESET_TIMEOUT);
  if (!EFI_ERROR(S)) { S=XhcWaitOpRegBit(X,XHC_USBSTS_OFFSET,XHC_USBSTS_CNR,FALSE,1000); }
  if (EFI_ERROR(S) || H->Io->Faulted || H->Io->Closed) { return Fault(H,EFI_ERROR(S) ? S : EFI_DEVICE_ERROR); }
  XhcInitSched(X);
  if (H->Io->Faulted || X->MemPool==NULL || X->DCBAA==NULL || X->CmdRing.RingSeg0==NULL ||
      X->EventRing.EventRingSeg0==NULL || X->EventRing.ERSTBase==NULL ||
      (X->MaxScratchpadBufs && (X->ScratchBuf==NULL || X->ScratchEntry==NULL || X->ScratchEntryMap==NULL))) {
    return Fault(H,EFI_OUT_OF_RESOURCES);
  }
  // Polling only. HSEE is explicit; source XhcSetHsee depends on PCI SERR.
  XhcClearOpRegBit(X,XHC_USBCMD_OFFSET,XHC_USBCMD_INTE);
  XhcClearRuntimeRegBit(X,XHC_IMAN_OFFSET,XHC_IMAN_IE);
  XhcSetOpRegBit(X,XHC_USBCMD_OFFSET,BIT3);
  if (Run) { S=XhcRunHC(X,1000); }
  return EFI_ERROR(S) || H->Io->Faulted ? Fault(H,EFI_ERROR(S) ? S : EFI_DEVICE_ERROR) : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Capability(EFI_USB2_HC_PROTOCOL *P, UINT8 *Speed, UINT8 *Ports, UINT8 *Dma64) {
  if (Speed==NULL || Ports==NULL || Dma64==NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  *Speed=EFI_USB_SPEED_HIGH; *Ports=H->PortCount; *Dma64=FALSE; return Leave(H,T,EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI State(EFI_USB2_HC_PROTOCOL *P, EFI_USB_HC_STATE *Out) {
  if (Out==NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  UINT32 Sts=0;
  if (H->Io->Quiescing) {
    S=DaliUsbXhciIoQuiesce(H->Io,1000000); Sts=1;
  } else { S=Read32(H,H->Io->OpOffset+4,&Sts); }
  if (!EFI_ERROR(S)) { *Out=(Sts&1) ? EfiUsbHcStateHalt : EfiUsbHcStateOperational; }
  return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI SetState(EFI_USB2_HC_PROTOCOL *P, EFI_USB_HC_STATE Value) {
  if (Value!=EfiUsbHcStateHalt && Value!=EfiUsbHcStateOperational) { return EFI_UNSUPPORTED; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (Value==EfiUsbHcStateHalt) { S=DaliUsbXhciIoQuiesce(H->Io,1000000); }
  else {
    if (H->Io->Quiescing) { S=DaliUsbXhciIoRearm(H->Io); }
    if (!EFI_ERROR(S)) {
      XhcSetOpRegBit(RAW(H),XHC_USBCMD_OFFSET,BIT3);
      S=XhcSetState(&RAW(H)->Usb2Hc,Value);
    }
  }
  if (EFI_ERROR(S)) { S=Fault(H,S); }
  return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI Reset(EFI_USB2_HC_PROTOCOL *P, UINT16 Attributes) {
  if (Attributes!=EFI_USB_HC_RESET_GLOBAL && Attributes!=EFI_USB_HC_RESET_HOST_CONTROLLER) { return EFI_UNSUPPORTED; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  S=DaliUsbXhciIoQuiesce(H->Io,1000000);
  if (!EFI_ERROR(S)) {
    FreeUsers(H);
    if (!H->Io->Faulted) { S=DaliUsbXhciIoRearm(H->Io); }
    else { S=EFI_DEVICE_ERROR; }
    if (!EFI_ERROR(S)) { S=Initialize(H,FALSE); }
  }
  if (EFI_ERROR(S)) { S=Fault(H,S); }
  return Leave(H,T,S);
}
STATIC BOOLEAN TransferArgs(UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet, UINTN Bytes) {
  return Address<=127 && !(Endpoint&0x70) && (Endpoint&15)!=0 && Speed<=EFI_USB_SPEED_HIGH &&
    Packet!=0 && Bytes<=1024*1024;
}
STATIC EFI_STATUS EFIAPI Control(EFI_USB2_HC_PROTOCOL *P, UINT8 Address, UINT8 Speed, UINTN Packet,
  EFI_USB_DEVICE_REQUEST *Request, EFI_USB_DATA_DIRECTION Direction, VOID *Data, UINTN *Bytes, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result) {
  if (Request==NULL || Bytes==NULL || Result==NULL || Address>127 || Speed>EFI_USB_SPEED_HIGH ||
      *Bytes>65535 || Request->Length>*Bytes || (Request->Length && Data==NULL)) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  S=XhcControlTransfer(&RAW(H)->Usb2Hc,Address,Speed,Packet,Request,Direction,Data,Bytes,
    Timeout==0 || Timeout>10000 ? 10000 : Timeout,Translator,Result);
  return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI Bulk(EFI_USB2_HC_PROTOCOL *P, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  UINT8 Count, VOID *Data[EFI_USB_MAX_BULK_BUFFER_NUM], UINTN *Bytes, UINT8 *Toggle, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result) {
  if (Bytes==NULL || Toggle==NULL || Count!=1 || !TransferArgs(Address,Endpoint,Speed,Packet,*Bytes)) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  S=XhcBulkTransfer(&RAW(H)->Usb2Hc,Address,Endpoint,Speed,Packet,Count,Data,Bytes,Toggle,
    Timeout==0 || Timeout>10000 ? 10000 : Timeout,Translator,Result);
  return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI Interrupt(EFI_USB2_HC_PROTOCOL *P, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  VOID *Data, UINTN *Bytes, UINT8 *Toggle, UINTN Timeout, EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result) {
  if (Bytes==NULL || Toggle==NULL || !TransferArgs(Address,Endpoint,Speed,Packet,*Bytes)) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  S=XhcSyncInterruptTransfer(&RAW(H)->Usb2Hc,Address,Endpoint,Speed,Packet,Data,Bytes,Toggle,
    Timeout==0 || Timeout>10000 ? 10000 : Timeout,Translator,Result);
  return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI Async(EFI_USB2_HC_PROTOCOL *P, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  BOOLEAN New, UINT8 *Toggle, UINTN Interval, UINTN Bytes, EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  EFI_ASYNC_USB_TRANSFER_CALLBACK Callback, VOID *Context) {
  (VOID)Translator;
  if (Toggle==NULL || Address>127 || !(Endpoint&128) || (Endpoint&0x70) || !(Endpoint&15) ||
      (New && (!TransferArgs(Address,Endpoint,Speed,Packet,Bytes) || Callback==NULL || !Bytes ||
       Interval<1 || Interval>255 || *Toggle>1 || Packet>1024))) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=Host(P); EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  USB_XHCI_INSTANCE *X=RAW(H); URB *Match=NULL; LIST_ENTRY *Entry;
  for (Entry=GetFirstNode(&X->AsyncIntTransfers); !IsNull(&X->AsyncIntTransfers,Entry); Entry=GetNextNode(&X->AsyncIntTransfers,Entry)) {
    URB *U=BASE_CR(Entry,URB,UrbList);
    if (U->Ep.BusAddr==Address && U->Ep.EpAddr==(Endpoint&15)) { Match=U; break; }
  }
  if (New) {
    if (Match!=NULL) { S=EFI_ALREADY_STARTED; }
    else if (!XhcBusDevAddrToSlotId(X,Address)) { S=EFI_NOT_FOUND; }
    else {
      URB *U=XhciInsertAsyncIntTransfer(X,Address,Endpoint,Speed,Packet,Bytes,Callback,Context);
      S=U==NULL ? EFI_OUT_OF_RESOURCES : RingIntTransferDoorBell(X,U);
      if (EFI_ERROR(S)) { S=Fault(H,S); }
    }
  } else if (Match==NULL) { S=EFI_NOT_FOUND; }
  else {
    // A successfully disabled slot no longer owns any endpoint DMA.
    S=XhcBusDevAddrToSlotId(X,Address) ? XhcDequeueTrbFromEndpoint(X,Match) : EFI_SUCCESS;
    if (S==EFI_ALREADY_STARTED) { S=EFI_SUCCESS; }
    if (!EFI_ERROR(S) && !H->Io->Faulted) {
      RemoveEntryList(&Match->UrbList); VOID *Data=Match->Data; XhcFreeUrb(X,Match); FreePool(Data);
    } else { S=Fault(H,S); }
  }
  return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI PortStatus(EFI_USB2_HC_PROTOCOL *P, UINT8 Port, EFI_USB_PORT_STATUS *Out) {
  DALI_USB_NATIVE_HOST *H=Host(P);
  if (Out==NULL || Port>=H->PortCount) { return EFI_INVALID_PARAMETER; }
  EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  S=XhcGetRootHubPortStatus(&RAW(H)->Usb2Hc,H->Ports[Port],Out); return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI SetPort(EFI_USB2_HC_PROTOCOL *P, UINT8 Port, EFI_USB_PORT_FEATURE Feature) {
  DALI_USB_NATIVE_HOST *H=Host(P); if (Port>=H->PortCount) { return EFI_INVALID_PARAMETER; }
  EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  S=XhcSetRootHubPortFeature(&RAW(H)->Usb2Hc,H->Ports[Port],Feature); return Leave(H,T,S);
}
STATIC EFI_STATUS EFIAPI ClearPort(EFI_USB2_HC_PROTOCOL *P, UINT8 Port, EFI_USB_PORT_FEATURE Feature) {
  DALI_USB_NATIVE_HOST *H=Host(P); if (Port>=H->PortCount) { return EFI_INVALID_PARAMETER; }
  EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  S=XhcClearRootHubPortFeature(&RAW(H)->Usb2Hc,H->Ports[Port],Feature); return Leave(H,T,S);
}
EFI_STATUS DaliUsbNativeHostPoll(DALI_USB_NATIVE_HOST *H) {
  if (CurrentTpl()!=TPL_APPLICATION && CurrentTpl()!=TPL_CALLBACK) { return EFI_UNSUPPORTED; }
  EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->CallbackDepth || H->Io->Quiescing) { return Leave(H,T,EFI_NOT_READY); }
  USB_XHCI_INSTANCE *X=RAW(H); LIST_ENTRY *Entry;
  for (Entry=GetFirstNode(&X->AsyncIntTransfers); !IsNull(&X->AsyncIntTransfers,Entry); Entry=GetNextNode(&X->AsyncIntTransfers,Entry)) {
    URB *U=BASE_CR(Entry,URB,UrbList);
    if (!XhcBusDevAddrToSlotId(X,U->Ep.BusAddr)) { continue; }
    XhcCheckUrbResult(X,U);
    if (H->Io->Faulted || H->Io->Closed) { return Leave(H,T,Fault(H,EFI_DEVICE_ERROR)); }
    if (!U->Finished) { continue; }
    S=XhcFlushAsyncIntMap(X,U);
    if (EFI_ERROR(S) || U->Completed>U->DataLen) { return Leave(H,T,Fault(H,EFI_DEVICE_ERROR)); }
    UINTN Bytes=U->Completed; UINT32 Result=U->Result; VOID *Data=NULL;
    if (Result==EFI_USB_NOERROR && Bytes) {
      Data=AllocateCopyPool(Bytes,U->Data);
      if (Data==NULL) { return Leave(H,T,Fault(H,EFI_OUT_OF_RESOURCES)); }
    }
    EFI_ASYNC_USB_TRANSFER_CALLBACK Callback=U->Callback; VOID *Context=U->Context;
    if (Result==EFI_USB_NOERROR) {
      S=XhcCreateTransferTrb(X,U);
      if (!EFI_ERROR(S)) { S=RingIntTransferDoorBell(X,U); }
      if (EFI_ERROR(S) || H->Io->Faulted || H->Io->Closed) {
        if (Data!=NULL) { FreePool(Data); }
        return Leave(H,T,Fault(H,EFI_ERROR(S) ? S : EFI_DEVICE_ERROR));
      }
    }
    H->CallbackDepth++;
    S=Leave(H,T,EFI_SUCCESS);
    if (!EFI_ERROR(S) && Callback!=NULL) { Callback(Data,Bytes,Context,Result); }
    // No URB/list pointer is used after callback: it may cancel any pipe.
    T=gBS->RaiseTPL(TPL_NOTIFY); H->CallbackDepth--; gBS->RestoreTPL(T);
    if (Data!=NULL) { FreePool(Data); }
    return S;
  }
  return Leave(H,T,EFI_SUCCESS);
}
STATIC VOID EFIAPI PollEvent(EFI_EVENT Event, VOID *Context) { (VOID)Event; DaliUsbNativeHostPoll(Context); }
STATIC VOID EFIAPI ExitEvent(EFI_EVENT Event, VOID *Context) {
  (VOID)Event; DALI_USB_NATIVE_HOST *H=Context;
  H->Accepting=FALSE;
  if (!H->Closed && H->Io!=NULL && H->Io->Owned && !H->Io->Closed) {
    if (EFI_ERROR(DaliUsbXhciIoQuiesce(H->Io,1000000))) { H->Faulted=TRUE; }
    H->Io->Closed=TRUE;
  }
  H->Closed=TRUE;
}
EFI_STATUS DaliUsbNativeHostStart(DALI_USB_NATIVE_HOST *H, DALI_USB_XHCI_IO *Io,
  EFI_DEVICE_PATH_PROTOCOL *Path, CONST UINT8 *Ports, UINT8 Count) {
  if (H==NULL || Io==NULL || Path==NULL || Ports==NULL || !Count || Count>16) { return EFI_INVALID_PARAMETER; }
  if (CurrentTpl()!=TPL_APPLICATION || H->Closed || Io->Closed) { return EFI_UNSUPPORTED; }
  if (H->Engine!=NULL || H->Io!=NULL) { return EFI_ALREADY_STARTED; }
  if (!Io->Initialized || !Io->Owned || Io->Faulted || Io->Quiescing || Io->RecordCount) { return EFI_NOT_READY; }
  H->Io=Io; H->DevicePath=Path; H->PortCount=Count; CopyMem(H->Ports,Ports,Count);
  H->Faulted=H->Prepared=FALSE; H->Accepting=TRUE; H->Published=FALSE;
  H->Engine=AllocateZeroPool(sizeof(ENGINE));
  if (H->Engine==NULL) { H->Io=NULL; return EFI_OUT_OF_RESOURCES; }
  ENGINE *E=H->Engine; E->Owner=H; USB_XHCI_INSTANCE *X=&E->Raw;
  X->Signature=XHCI_INSTANCE_SIG; X->PciIo=&Io->Io; X->Usb2Hc=gXhciUsb2HcTemplate;
  InitializeListHead(&X->AsyncIntTransfers);
  H->Protocol=gXhciUsb2HcTemplate; H->Protocol.MajorRevision=2; H->Protocol.MinorRevision=0;
  H->Protocol.GetCapability=Capability; H->Protocol.Reset=Reset; H->Protocol.GetState=State; H->Protocol.SetState=SetState;
  H->Protocol.ControlTransfer=Control; H->Protocol.BulkTransfer=Bulk; H->Protocol.SyncInterruptTransfer=Interrupt;
  H->Protocol.AsyncInterruptTransfer=Async; H->Protocol.GetRootHubPortStatus=PortStatus;
  H->Protocol.SetRootHubPortFeature=SetPort; H->Protocol.ClearRootHubPortFeature=ClearPort;
  EFI_TPL T; EFI_STATUS S=Enter(H,FALSE,&T); if (EFI_ERROR(S)) { return S; }
  S=Capabilities(H);
  if (!EFI_ERROR(S)) { S=Initialize(H,TRUE); }
  if (EFI_ERROR(S)) { S=Fault(H,S); }
  S=Leave(H,T,S); if (EFI_ERROR(S)) { return S; }
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,PollEvent,H,&H->Timer);
  if (!EFI_ERROR(S)) { S=gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_NOTIFY,ExitEvent,H,&H->ExitEvent); }
  if (!EFI_ERROR(S)) { S=gBS->SetTimer(H->Timer,TimerPeriodic,EFI_TIMER_PERIOD_MILLISECONDS(1)); }
  if (!EFI_ERROR(S)) {
    S=gBS->InstallMultipleProtocolInterfaces(&H->Handle,&gEfiUsb2HcProtocolGuid,&H->Protocol,
      &gEfiDevicePathProtocolGuid,Path,NULL);
    if (!EFI_ERROR(S)) { H->Published=TRUE; }
  }
  return EFI_ERROR(S) ? Fault(H,S) : EFI_SUCCESS;
}
EFI_STATUS DaliUsbNativeHostPrepareHandoff(DALI_USB_NATIVE_HOST *H) {
  if (CurrentTpl()!=TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  EFI_TPL T; EFI_STATUS S=Enter(H,TRUE,&T); if (EFI_ERROR(S)) { return S; }
  if (H->CallbackDepth) { return Leave(H,T,EFI_NOT_READY); }
  H->Accepting=FALSE; H->Prepared=TRUE;
  S=DaliUsbXhciIoQuiesce(H->Io,1000000);
  if (EFI_ERROR(S)) { S=Fault(H,S); }
  // Do not use Leave: a prior retained software fault is compatible with
  // successful fresh DMA halt, and no map-changing cleanup is allowed here.
  H->Busy=0; gBS->RestoreTPL(T); return S;
}
EFI_STATUS DaliUsbNativeHostStop(DALI_USB_NATIVE_HOST *H) {
  if (H==NULL || CurrentTpl()!=TPL_APPLICATION || H->Closed) { return EFI_UNSUPPORTED; }
  if (H->CallbackDepth || H->Busy) { return EFI_NOT_READY; }
  if (H->Published) {
    EFI_STATUS S=gBS->DisconnectController(H->Handle,NULL,NULL);
    if (EFI_ERROR(S)) { return S; }
  }
  EFI_TPL T; EFI_STATUS S=Enter(H,TRUE,&T); if (EFI_ERROR(S)) { return S; }
  H->Accepting=FALSE; H->Prepared=TRUE;
  S=DaliUsbXhciIoQuiesce(H->Io,1000000);
  H->Busy=0; gBS->RestoreTPL(T); if (EFI_ERROR(S)) { return Fault(H,S); }
  if (H->Published) {
    S=gBS->UninstallMultipleProtocolInterfaces(H->Handle,&gEfiUsb2HcProtocolGuid,&H->Protocol,
      &gEfiDevicePathProtocolGuid,H->DevicePath,NULL);
    if (EFI_ERROR(S)) { return S; }
    H->Published=FALSE; H->Handle=NULL;
  }
  if (H->Timer!=NULL) {
    S=gBS->CloseEvent(H->Timer); if (EFI_ERROR(S)) { return S; } H->Timer=NULL;
  }
  if (H->ExitEvent!=NULL) {
    S=gBS->CloseEvent(H->ExitEvent); if (EFI_ERROR(S)) { return S; } H->ExitEvent=NULL;
  }
  T=gBS->RaiseTPL(TPL_NOTIFY); FreeUsers(H); FreePool(H->Engine);
  H->Engine=NULL; H->Io=NULL; H->DevicePath=NULL; H->Busy=0; gBS->RestoreTPL(T);
  // Even ignored source unmap/free errors retain independent IO records.
  // The caller must separately retry IO Close before dropping its providers.
  return EFI_SUCCESS;
}
