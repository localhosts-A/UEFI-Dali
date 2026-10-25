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
EFI_STATUS DaliNativeXhciRollbackEndpointConfiguration(USB_XHCI_INSTANCE *, UINT8);
#define DALI_MTK_MAX_ENDPOINTS 128U
#define DALI_MTK_MAX_TTS 128U
#define DALI_MTK_ESIT_SLOTS 64U
#define DALI_MTK_HS_BW_LIMIT 6144U
#define DALI_MTK_FS_BW_LIMIT 188U

typedef struct {
  BOOLEAN Used, HasTt;
  UINT8 Slot, Dci, RootPort, Speed, Interface, Alternate;
  UINT8 TtHubSlot, TtPort, TtIndex, Pkts, CsCount, Offset;
  UINT8 Repeat, BurstMode, Esit, NumEsit, NumBudget;
  UINT16 MaxPacket;
  UINT32 Cost;
} MTK_ENDPOINT;

typedef struct {
  BOOLEAN Used;
  UINT8 HubSlot, Port;
  UINT16 References;
  UINT32 FsBus[64];
} MTK_TT;

typedef struct {
  BOOLEAN Active, AllInterfaces;
  UINT8 Slot, Interface, Count;
  MTK_ENDPOINT Backup[31];
} MTK_TRANSACTION;

typedef struct {
  UINT32 RootBus[256][64];
  MTK_TT Tt[DALI_MTK_MAX_TTS];
  MTK_ENDPOINT Endpoints[DALI_MTK_MAX_ENDPOINTS];
  MTK_TRANSACTION Transaction;
} MTK_SCHEDULER;

typedef struct {
  USB_XHCI_INSTANCE Raw;
  DALI_USB_NATIVE_HOST *Owner;
  UINT32 LastShortEndpointMask[256];
  MTK_SCHEDULER Mtk;
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
STATIC ENGINE *NativeEngine(USB_XHCI_INSTANCE *X) {
  if (X==NULL) { return NULL; }
  ENGINE *E=BASE_CR(X,ENGINE,Raw);
  if (E->Owner==NULL || E->Owner->Engine!=E || E->Owner->Closed) { return NULL; }
  return E;
}

STATIC UINT8 MtkFindTt(ENGINE *E, UINT8 HubSlot, UINT8 Port, BOOLEAN Create) {
  for (UINTN I=0; I<DALI_MTK_MAX_TTS; I++) {
    MTK_TT *T=(MTK_TT *)&E->Mtk.Tt[I];
    if (T->Used && T->HubSlot==HubSlot && T->Port==Port) { return (UINT8)I; }
  }
  if (!Create) { return 0xff; }
  for (UINTN I=0; I<DALI_MTK_MAX_TTS; I++) {
    MTK_TT *T=(MTK_TT *)&E->Mtk.Tt[I];
    if (!T->Used) {
      ZeroMem(T,sizeof *T); T->Used=TRUE; T->HubSlot=HubSlot; T->Port=Port;
      return (UINT8)I;
    }
  }
  return 0xff;
}

STATIC BOOLEAN MtkRecordFits(ENGINE *E, CONST MTK_ENDPOINT *R, UINT8 Offset) {
  MTK_TT *T=NULL;
  if (R->HasTt) {
    UINT8 Index=MtkFindTt(E,R->TtHubSlot,R->TtPort,FALSE);
    if (Index!=0xff) { T=(MTK_TT *)&E->Mtk.Tt[Index]; }
  }
  for (UINTN I=0; I<R->NumEsit; I++) {
    UINTN Base=(Offset+I*R->Esit)&63U;
    for (UINTN J=0; J<R->NumBudget; J++) {
      UINTN Index=(Base+J)&63U;
      UINT32 Root=E->Mtk.RootBus[R->RootPort][Index];
      if (Root>DALI_MTK_HS_BW_LIMIT-R->Cost) { return FALSE; }
      if (T!=NULL && T->FsBus[Index]>DALI_MTK_FS_BW_LIMIT-R->Cost) { return FALSE; }
    }
  }
  return TRUE;
}

STATIC BOOLEAN MtkUpdateRecord(ENGINE *E, MTK_ENDPOINT *R, BOOLEAN Add) {
  MTK_TT *T=NULL;
  if (R->HasTt) {
    UINT8 Index=Add ? MtkFindTt(E,R->TtHubSlot,R->TtPort,TRUE) : R->TtIndex;
    if (Index==0xff || Index>=DALI_MTK_MAX_TTS) { return FALSE; }
    T=(MTK_TT *)&E->Mtk.Tt[Index];
    if (!T->Used || T->HubSlot!=R->TtHubSlot || T->Port!=R->TtPort) { return FALSE; }
    if (Add) { R->TtIndex=Index; T->References++; }
    else if (T->References==0) { return FALSE; }
  }
  for (UINTN I=0; I<R->NumEsit; I++) {
    UINTN Base=(R->Offset+I*R->Esit)&63U;
    for (UINTN J=0; J<R->NumBudget; J++) {
      UINTN Index=(Base+J)&63U;
      if (Add) {
        E->Mtk.RootBus[R->RootPort][Index]+=R->Cost;
        if (T!=NULL) { T->FsBus[Index]+=R->Cost; }
      } else {
        if (E->Mtk.RootBus[R->RootPort][Index]<R->Cost ||
            (T!=NULL && T->FsBus[Index]<R->Cost)) { return FALSE; }
        E->Mtk.RootBus[R->RootPort][Index]-=R->Cost;
        if (T!=NULL) { T->FsBus[Index]-=R->Cost; }
      }
    }
  }
  if (T!=NULL && !Add) {
    T->References--;
    if (T->References==0) { ZeroMem(T,sizeof *T); }
  }
  return TRUE;
}

STATIC BOOLEAN MtkAddRecord(ENGINE *E, MTK_ENDPOINT *R) {
  UINTN FreeIndex=DALI_MTK_MAX_ENDPOINTS;
  for (UINTN I=0; I<DALI_MTK_MAX_ENDPOINTS; I++) {
    MTK_ENDPOINT *P=(MTK_ENDPOINT *)&E->Mtk.Endpoints[I];
    if (P->Used && P->Slot==R->Slot && P->Dci==R->Dci) { return FALSE; }
    if (!P->Used && FreeIndex==DALI_MTK_MAX_ENDPOINTS) { FreeIndex=I; }
  }
  if (FreeIndex==DALI_MTK_MAX_ENDPOINTS || !MtkRecordFits(E,R,R->Offset)) { return FALSE; }
  if (!MtkUpdateRecord(E,R,TRUE)) { return FALSE; }
  R->Used=TRUE;
  CopyMem(&E->Mtk.Endpoints[FreeIndex],R,sizeof *R);
  return TRUE;
}

STATIC BOOLEAN MtkRemoveRecord(ENGINE *E, UINTN Index) {
  MTK_ENDPOINT *R=(MTK_ENDPOINT *)&E->Mtk.Endpoints[Index];
  if (!R->Used || !MtkUpdateRecord(E,R,FALSE)) { return FALSE; }
  ZeroMem(R,sizeof *R);
  return TRUE;
}

STATIC BOOLEAN MtkInTransaction(ENGINE *E, UINT8 Slot, UINT8 Interface) {
  MTK_TRANSACTION *T=(MTK_TRANSACTION *)&E->Mtk.Transaction;
  return T->Active && T->Slot==Slot && (T->AllInterfaces || T->Interface==Interface);
}

EFI_STATUS DaliNativeXhciBeginEndpointConfiguration(USB_XHCI_INSTANCE *X, UINT8 Slot,
                                                     BOOLEAN AllInterfaces, UINT8 Interface) {
  ENGINE *E=NativeEngine(X);
  if (E==NULL || Slot==0 || !X->UsbDevContext[Slot].Enabled ||
      X->UsbDevContext[Slot].SlotId!=Slot) { return EFI_INVALID_PARAMETER; }
  MTK_TRANSACTION *T=(MTK_TRANSACTION *)&E->Mtk.Transaction;
  if (T->Active) { return EFI_NOT_READY; }
  UINTN Count=0;
  for (UINTN I=0; I<DALI_MTK_MAX_ENDPOINTS; I++) {
    MTK_ENDPOINT *R=(MTK_ENDPOINT *)&E->Mtk.Endpoints[I];
    if (R->Used && R->Slot==Slot && (AllInterfaces || R->Interface==Interface)) { Count++; }
  }
  if (Count>31) { return EFI_OUT_OF_RESOURCES; }
  ZeroMem(T,sizeof *T); T->Active=TRUE; T->AllInterfaces=AllInterfaces; T->Slot=Slot;
  T->Interface=Interface;
  UINTN Out=0;
  for (UINTN I=0; I<DALI_MTK_MAX_ENDPOINTS; I++) {
    MTK_ENDPOINT *R=(MTK_ENDPOINT *)&E->Mtk.Endpoints[I];
    if (R->Used && R->Slot==Slot && (AllInterfaces || R->Interface==Interface)) {
      CopyMem(&T->Backup[Out++],R,sizeof *R);
      if (!MtkRemoveRecord(E,I)) { T->Count=(UINT8)Out; return DaliNativeXhciRollbackEndpointConfiguration(X,Slot); }
    }
  }
  T->Count=(UINT8)Out;
  return EFI_SUCCESS;
}

EFI_STATUS DaliNativeXhciCommitEndpointConfiguration(USB_XHCI_INSTANCE *X, UINT8 Slot) {
  ENGINE *E=NativeEngine(X);
  if (E==NULL) { return EFI_UNSUPPORTED; }
  MTK_TRANSACTION *T=(MTK_TRANSACTION *)&E->Mtk.Transaction;
  if (!T->Active || T->Slot!=Slot) { return EFI_NOT_STARTED; }
  ZeroMem(T,sizeof *T);
  return EFI_SUCCESS;
}

EFI_STATUS DaliNativeXhciRollbackEndpointConfiguration(USB_XHCI_INSTANCE *X, UINT8 Slot) {
  ENGINE *E=NativeEngine(X);
  if (E==NULL) { return EFI_UNSUPPORTED; }
  MTK_TRANSACTION *T=(MTK_TRANSACTION *)&E->Mtk.Transaction;
  if (!T->Active || T->Slot!=Slot) { return EFI_NOT_STARTED; }
  for (UINTN I=0; I<DALI_MTK_MAX_ENDPOINTS; I++) {
    MTK_ENDPOINT *R=(MTK_ENDPOINT *)&E->Mtk.Endpoints[I];
    if (R->Used && R->Slot==Slot && (T->AllInterfaces || R->Interface==T->Interface)) {
      if (!MtkRemoveRecord(E,I)) { return Fault(E->Owner,EFI_DEVICE_ERROR); }
    }
  }
  UINT8 Count=T->Count;
  for (UINTN I=0; I<Count; I++) {
    MTK_ENDPOINT R; CopyMem(&R,&T->Backup[I],sizeof R);
    if (!MtkAddRecord(E,&R)) { return Fault(E->Owner,EFI_DEVICE_ERROR); }
  }
  ZeroMem(T,sizeof *T);
  return EFI_SUCCESS;
}

EFI_STATUS DaliNativeXhciRemoveSlotReservations(USB_XHCI_INSTANCE *X, UINT8 Slot) {
  ENGINE *E=NativeEngine(X);
  if (E==NULL) { return EFI_UNSUPPORTED; }
  MTK_TRANSACTION *T=(MTK_TRANSACTION *)&E->Mtk.Transaction;
  if (T->Active && T->Slot==Slot) { return Fault(E->Owner,EFI_NOT_READY); }
  for (UINTN I=0; I<DALI_MTK_MAX_ENDPOINTS; I++) {
    MTK_ENDPOINT *R=(MTK_ENDPOINT *)&E->Mtk.Endpoints[I];
    if (R->Used && R->Slot==Slot && !MtkRemoveRecord(E,I)) { return Fault(E->Owner,EFI_DEVICE_ERROR); }
  }
  return EFI_SUCCESS;
}

BOOLEAN DaliNativeXhciScheduleEndpoint(USB_XHCI_INSTANCE *X, UINT8 Slot, UINT8 Dci,
                                       UINT8 Speed, CONST USB_INTERFACE_DESCRIPTOR *IfDesc,
                                       CONST USB_ENDPOINT_DESCRIPTOR *EpDesc, UINT8 Interval,
                                       UINT16 *PayloadOut, UINT8 *BurstOut, UINT16 *EsitPayloadOut,
                                       UINT32 *MtkWord0, UINT32 *MtkWord1) {
  ENGINE *E=NativeEngine(X);
  if (E==NULL || IfDesc==NULL || EpDesc==NULL ||
      !MtkInTransaction(E,Slot,IfDesc->InterfaceNumber) ||
      PayloadOut==NULL || BurstOut==NULL || EsitPayloadOut==NULL || MtkWord0==NULL || MtkWord1==NULL) { return FALSE; }
  UINT16 Raw=EpDesc->MaxPacketSize;
  UINT16 Payload=(UINT16)(Raw&0x07ffU);
  UINT8 Transactions=1;
  if (Speed==EFI_USB_SPEED_HIGH) {
    UINT8 Mult=(UINT8)((Raw>>11)&3U);
    if ((Raw&0xe000U)!=0 || Mult==3) { return FALSE; }
    Transactions=(UINT8)(Mult+1U);
  } else if (Speed==EFI_USB_SPEED_FULL || Speed==EFI_USB_SPEED_LOW) {
    if ((Raw&0xf800U)!=0 || Payload>(Speed==EFI_USB_SPEED_LOW ? 8U : 64U)) { return FALSE; }
  } else { return FALSE; }
  if (Payload==0 || Payload>(Speed==EFI_USB_SPEED_HIGH ? 1024U : 1023U) || Interval>15U) { return FALSE; }

  MTK_ENDPOINT R; ZeroMem(&R,sizeof R);
  USB_DEV_CONTEXT *Device=&X->UsbDevContext[Slot];
  SLOT_CONTEXT *SlotContext=(SLOT_CONTEXT *)Device->OutputContext;
  if (SlotContext==NULL || SlotContext->RootHubPortNum==0 || SlotContext->RootHubPortNum> X->HcSParams1.Data.MaxPorts) { return FALSE; }
  R.Slot=Slot; R.Dci=Dci; R.RootPort=SlotContext->RootHubPortNum; R.Speed=Speed;
  R.Interface=IfDesc->InterfaceNumber; R.Alternate=IfDesc->AlternateSetting;
  R.MaxPacket=Payload; R.Pkts=Transactions;
  UINTN Esit=1U<<Interval;
  if (Esit>DALI_MTK_ESIT_SLOTS) { Esit=DALI_MTK_ESIT_SLOTS; }
  R.Esit=(UINT8)Esit;
  R.NumEsit=(UINT8)(DALI_MTK_ESIT_SLOTS/R.Esit); R.NumBudget=1;
  R.Cost=(UINT32)Payload*Transactions;
  if (Speed==EFI_USB_SPEED_HIGH) {
    R.HasTt=FALSE;
  } else {
    R.Pkts=1;
    R.Cost=Payload>DALI_MTK_FS_BW_LIMIT ? DALI_MTK_FS_BW_LIMIT : Payload;
    if (SlotContext->TTHubSlotId!=0) {
      UINT8 HubSlot=SlotContext->TTHubSlotId;
      if (!X->UsbDevContext[HubSlot].Enabled || X->UsbDevContext[HubSlot].SlotId!=HubSlot ||
          X->UsbDevContext[HubSlot].OutputContext==NULL || SlotContext->TTPortNum==0 || SlotContext->TTPortNum>127) { return FALSE; }
      SLOT_CONTEXT *HubContext=(SLOT_CONTEXT *)X->UsbDevContext[HubSlot].OutputContext;
      if (!HubContext->Hub || HubContext->Speed!=(EFI_USB_SPEED_HIGH+1) ||
          HubContext->RootHubPortNum!=R.RootPort || SlotContext->TTPortNum>HubContext->PortNum) { return FALSE; }
      R.HasTt=TRUE; R.TtHubSlot=HubSlot; R.TtPort=HubContext->MTT ? SlotContext->TTPortNum : 0;
      R.CsCount=(UINT8)((Payload+DALI_MTK_FS_BW_LIMIT-1U)/DALI_MTK_FS_BW_LIMIT);
      if (R.CsCount>7) { R.CsCount=7; }
      R.NumBudget=R.CsCount>R.Esit ? R.Esit : R.CsCount;
    } else {
      *PayloadOut=Payload; *BurstOut=0; *EsitPayloadOut=Payload;
      *MtkWord0=1; *MtkWord1=0;
      return TRUE;
    }
  }

  UINT8 BestOffset=0xff; UINT32 BestWorst=~(UINT32)0;
  UINT8 OriginalBudget=R.NumBudget;
  for (UINTN Offset=0; Offset<R.Esit; Offset++) {
    if (R.HasTt) {
      UINTN StartSs=Offset%8U;
      UINTN StartCs,LastCs,CsCount=(Payload+DALI_MTK_FS_BW_LIMIT-1U)/DALI_MTK_FS_BW_LIMIT;
      if (StartSs==6U) { continue; }
      StartCs=(StartSs+2U)%8U; LastCs=StartCs+CsCount-1U;
      if (LastCs>7U) { continue; }
      if (CsCount>7U) { CsCount=7U; }
      R.CsCount=(UINT8)CsCount; R.NumBudget=(UINT8)(CsCount>R.Esit ? R.Esit : CsCount);
    } else { R.CsCount=0; R.NumBudget=1; }
    if (!MtkRecordFits(E,&R,(UINT8)Offset)) { continue; }
    UINT32 Worst=0;
    for (UINTN I=0; I<R.NumEsit; I++) {
      UINTN Base=(Offset+I*R.Esit)&63U;
      for (UINTN J=0; J<R.NumBudget; J++) {
        UINT32 Used=E->Mtk.RootBus[R.RootPort][(Base+J)&63U]+R.Cost;
        if (Used>Worst) { Worst=Used; }
      }
    }
    if (Worst<BestWorst) { BestWorst=Worst; BestOffset=(UINT8)Offset; }
    if (R.HasTt || Worst==0) { break; }
  }
  if (BestOffset==0xff) { return FALSE; }
  R.Offset=BestOffset; R.NumBudget=OriginalBudget;
  if (R.HasTt) {
    UINTN CsCount=(Payload+DALI_MTK_FS_BW_LIMIT-1U)/DALI_MTK_FS_BW_LIMIT;
    R.CsCount=(UINT8)(CsCount>7U ? 7U : CsCount);
    R.NumBudget=(UINT8)(R.CsCount>R.Esit ? R.Esit : R.CsCount);
  }
  if (!MtkAddRecord(E,&R)) { return FALSE; }
  *PayloadOut=Payload; *BurstOut=(UINT8)(Transactions-1U);
  *EsitPayloadOut=(UINT16)((UINT32)Payload*Transactions);
  *MtkWord0=((UINT32)R.Pkts&0x7fU)|(((UINT32)R.CsCount&7U)<<8)|(((UINT32)R.BurstMode&1U)<<11);
  *MtkWord1=((UINT32)R.Offset&0x3fffU)|(((UINT32)R.Repeat&0x7fffU)<<16);
  return TRUE;
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
  ZeroMem(&((ENGINE *)H->Engine)->Mtk, sizeof ((ENGINE *)H->Engine)->Mtk);
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
STATIC EFI_STATUS EFIAPI HandoffProtocolPrepare(DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL *This) {
  if (This == NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST *H=BASE_CR(This,DALI_USB_NATIVE_HOST,HandoffProtocol);
  if (H->HandoffProtocol.PrepareHandoff!=HandoffProtocolPrepare || H->Engine==NULL || H->Closed) {
    return EFI_UNSUPPORTED;
  }
  return DaliUsbNativeHostPrepareHandoff(H);
}
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
  H->HandoffProtocol.Revision=DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL_REVISION;
  H->HandoffProtocol.PrepareHandoff=HandoffProtocolPrepare;
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
      &gEfiDevicePathProtocolGuid,Path,&gDaliUsbNativeHostHandoffProtocolGuid,&H->HandoffProtocol,NULL);
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
      &gEfiDevicePathProtocolGuid,H->DevicePath,&gDaliUsbNativeHostHandoffProtocolGuid,&H->HandoffProtocol,NULL);
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
