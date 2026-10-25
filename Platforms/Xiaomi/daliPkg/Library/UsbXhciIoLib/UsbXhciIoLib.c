// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbXhciIoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#define MAX_BUFFER (16U * 1024U * 1024U)
#define MAX_TOTAL (64U * 1024U * 1024U)
#define MAX_RECORDS 1024U
typedef enum { Allocation, Streaming, CommonMap } KIND;
typedef struct RECORD RECORD;
struct RECORD {
  RECORD *Next, *Parent;
  KIND Kind;
  DALI_USB_XHCI_DMA_BUFFER Buffer;
  VOID *Host;
  UINTN Bytes, First, LiveBytes, References, Accounted;
  DALI_USB_XHCI_DMA_DIRECTION Direction;
};
STATIC DALI_USB_XHCI_IO *Owner(EFI_PCI_IO_PROTOCOL *Io) {
  return BASE_CR(Io,DALI_USB_XHCI_IO,Io);
}
STATIC EFI_STATUS Fail(DALI_USB_XHCI_IO *C, EFI_STATUS Status) {
  C->Faulted=TRUE; return Status;
}
STATIC EFI_STATUS Proof(DALI_USB_XHCI_IO *C, DALI_USB_XHCI_IO_PHASE Phase) {
  if (C->Closed) { return EFI_UNSUPPORTED; }
  EFI_STATUS Status=C->Guard(C->GuardContext,Phase);
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  return EFI_ERROR(Status) ? Fail(C,Status) : EFI_SUCCESS;
}
STATIC EFI_STATUS Enter(DALI_USB_XHCI_IO *C, BOOLEAN Cleanup) {
  if (!C->Initialized || C->Closed) { return EFI_UNSUPPORTED; }
  if ((C->Faulted || C->Quiescing) && !(Cleanup && C->Quiescing && C->HaltProven)) { return EFI_DEVICE_ERROR; }
  return Proof(C,DaliXhciValidate);
}
STATIC RECORD *New(DALI_USB_XHCI_IO *C, KIND Kind) {
  if (C->RecordCount>=MAX_RECORDS) { Fail(C,EFI_OUT_OF_RESOURCES); return NULL; }
  RECORD *R=AllocateZeroPool(sizeof *R);
  if (R==NULL) { Fail(C,EFI_OUT_OF_RESOURCES); return NULL; }
  R->Kind=Kind; R->Next=C->Records; C->Records=R; C->RecordCount++; return R;
}
STATIC VOID Remove(DALI_USB_XHCI_IO *C, RECORD *R) {
  RECORD **At=(RECORD **)&C->Records;
  while (*At!=R) { At=&(*At)->Next; }
  *At=R->Next; C->RecordCount--; C->AllocatedBytes-=R->Accounted; FreePool(R);
}
STATIC BOOLEAN Overlap(UINT64 A, UINT64 N, UINT64 B, UINT64 M) {
  return A<B ? B-A<N : A-B<M;
}
STATIC EFI_STATUS Allocate(DALI_USB_XHCI_IO *C, RECORD *R, UINTN Bytes, BOOLEAN Common) {
  if (!Bytes || Bytes>MAX_BUFFER || Bytes>MAX_TOTAL-C->AllocatedBytes) { return Fail(C,EFI_OUT_OF_RESOURCES); }
  UINTN Alignment=Common ? 0x10000 : EFI_PAGE_SIZE;
  EFI_STATUS Status=C->Dma.Allocate(C->Dma.Context,Bytes,Alignment,0xffffffff,&R->Buffer);
  // R is already owned before the callback: an applied/error allocation or
  // EBS cannot lose the provider's persistent partial-ownership descriptor.
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  if (EFI_ERROR(Status)) { return Fail(C,Status); }
  DALI_USB_XHCI_DMA_BUFFER *B=&R->Buffer;
  if (B->Cpu==NULL || B->Size!=Bytes || ((UINTN)B->Cpu&(Alignment-1)) ||
      B->Address>0xffffffff || (B->Address&(Alignment-1)) ||
      Bytes-1>0xffffffff-B->Address || (UINTN)B->Cpu>MAX_UINTN-(Bytes-1) ||
      (Common && !B->Coherent)) { return Fail(C,EFI_UNSUPPORTED); }
  for (RECORD *P=C->Records; P!=NULL; P=P->Next) {
    if (P==R || P->Kind==CommonMap || P->Buffer.Cpu==NULL) { continue; }
    if (Overlap((UINTN)B->Cpu,Bytes,(UINTN)P->Buffer.Cpu,P->Buffer.Size) ||
        Overlap(B->Address,Bytes,P->Buffer.Address,P->Buffer.Size)) { return Fail(C,EFI_ACCESS_DENIED); }
  }
  R->Accounted=Bytes; C->AllocatedBytes+=Bytes; R->LiveBytes=Bytes;
  return Proof(C,DaliXhciValidate);
}
STATIC EFI_STATUS Sync(DALI_USB_XHCI_IO *C, RECORD *R, UINTN Offset, UINTN Bytes,
  DALI_USB_XHCI_DMA_DIRECTION Direction, BOOLEAN Device) {
  EFI_STATUS Status=C->Dma.Sync(C->Dma.Context,&R->Buffer,Offset,Bytes,Direction,Device);
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  if (EFI_ERROR(Status)) { return Fail(C,Status); }
  return Proof(C,DaliXhciValidate);
}
STATIC EFI_STATUS Release(DALI_USB_XHCI_IO *C, RECORD *R) {
  EFI_STATUS Status=Proof(C,DaliXhciValidate);
  if (EFI_ERROR(Status)) { return Status; }
  if (R->Buffer.Cpu!=NULL || R->Buffer.Cookie!=NULL || R->Buffer.Size) {
    Status=C->Dma.Free(C->Dma.Context,&R->Buffer);
  }
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  if (EFI_ERROR(Status)) { return Fail(C,Status); }
  Remove(C,R); return Proof(C,DaliXhciValidate);
}
STATIC EFI_STATUS Read(DALI_USB_XHCI_IO *C, UINTN Offset, UINTN Width, VOID *Out) {
  EFI_STATUS Status=Proof(C,DaliXhciValidate);
  if (EFI_ERROR(Status)) { return Status; }
  MemoryFence(); UINT32 Value=Width==1 ? MmioRead8(C->Base+Offset) : MmioRead32(C->Base+Offset); MemoryFence();
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  Status=Proof(C,DaliXhciValidate);
  if (!EFI_ERROR(Status)) { CopyMem(Out,&Value,Width); }
  return Status;
}
STATIC EFI_STATUS EFIAPI Mem(EFI_PCI_IO_PROTOCOL *Io, EFI_PCI_IO_PROTOCOL_WIDTH Width,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Buffer, BOOLEAN Write) {
  DALI_USB_XHCI_IO *C=Owner(Io);
  if (!C->Initialized || C->Closed) { return EFI_UNSUPPORTED; }
  if (Buffer==NULL || Count!=1 || Bar!=0 || (Width!=EfiPciIoWidthUint8 && Width!=EfiPciIoWidthUint32)) {
    return Fail(C,EFI_INVALID_PARAMETER);
  }
  UINTN Size=Width==EfiPciIoWidthUint8 ? 1 : 4;
  if (Offset>C->Size-Size || (Offset&(Size-1))) { return Fail(C,EFI_INVALID_PARAMETER); }
  if (C->Quiescing || C->Faulted) { return EFI_DEVICE_ERROR; }
  if (!Write) { return Read(C,(UINTN)Offset,Size,Buffer); }
  EFI_STATUS Status=Proof(C,DaliXhciValidate);
  if (EFI_ERROR(Status)) { return Status; }
  UINT32 Value=0; CopyMem(&Value,Buffer,Size);
  C->HaltProven=FALSE; MemoryFence();
  if (Size==1) { MmioWrite8(C->Base+(UINTN)Offset,(UINT8)Value); }
  else { MmioWrite32(C->Base+(UINTN)Offset,Value); }
  MemoryFence();
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  return Proof(C,DaliXhciValidate);
}
STATIC EFI_STATUS EFIAPI MemRead(EFI_PCI_IO_PROTOCOL *Io, EFI_PCI_IO_PROTOCOL_WIDTH Width,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Buffer) { return Mem(Io,Width,Bar,Offset,Count,Buffer,FALSE); }
STATIC EFI_STATUS EFIAPI MemWrite(EFI_PCI_IO_PROTOCOL *Io, EFI_PCI_IO_PROTOCOL_WIDTH Width,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Buffer) { return Mem(Io,Width,Bar,Offset,Count,Buffer,TRUE); }
STATIC EFI_STATUS EFIAPI PciRead(EFI_PCI_IO_PROTOCOL *Io, EFI_PCI_IO_PROTOCOL_WIDTH Width,
  UINT32 Offset, UINTN Count, VOID *Buffer) {
  (VOID)Io; (VOID)Width; (VOID)Offset; (VOID)Count; (VOID)Buffer; return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI AllocateBuffer(EFI_PCI_IO_PROTOCOL *Io, EFI_ALLOCATE_TYPE Type,
  EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **Out, UINT64 Attributes) {
  DALI_USB_XHCI_IO *C=Owner(Io);
  if (Out==NULL || !Pages || Pages>MAX_BUFFER/EFI_PAGE_SIZE || Type!=AllocateAnyPages ||
      MemoryType!=EfiBootServicesData || Attributes) { return Fail(C,EFI_INVALID_PARAMETER); }
  EFI_STATUS Status=Enter(C,FALSE); if (EFI_ERROR(Status)) { return Status; }
  RECORD *R=New(C,Allocation); if (R==NULL) { return EFI_OUT_OF_RESOURCES; }
  Status=Allocate(C,R,EFI_PAGES_TO_SIZE(Pages),TRUE);
  if (!EFI_ERROR(Status)) { *Out=R->Buffer.Cpu; }
  return Status;
}
STATIC EFI_STATUS EFIAPI FreeBuffer(EFI_PCI_IO_PROTOCOL *Io, UINTN Pages, VOID *Host) {
  DALI_USB_XHCI_IO *C=Owner(Io);
  if (!Pages || Pages>MAX_BUFFER/EFI_PAGE_SIZE || Host==NULL || ((UINTN)Host&(EFI_PAGE_SIZE-1))) {
    return Fail(C,EFI_INVALID_PARAMETER);
  }
  EFI_STATUS Status=Enter(C,TRUE); if (EFI_ERROR(Status)) { return Status; }
  UINTN Bytes=EFI_PAGES_TO_SIZE(Pages);
  for (RECORD *R=C->Records; R!=NULL; R=R->Next) {
    if (R->Kind!=Allocation || (UINTN)Host<(UINTN)R->Buffer.Cpu) { continue; }
    UINTN Offset=(UINTN)Host-(UINTN)R->Buffer.Cpu;
    if (Offset<R->First || Offset-R->First>R->LiveBytes || Bytes>R->LiveBytes-(Offset-R->First)) { continue; }
    if (R->References || (Offset!=R->First && Bytes!=R->LiveBytes-(Offset-R->First))) { return Fail(C,EFI_ACCESS_DENIED); }
    for (RECORD *Map=C->Records; Map!=NULL; Map=Map->Next) {
      if (Map->Kind==Streaming && Overlap((UINTN)Host,Bytes,(UINTN)Map->Host,Map->Bytes)) {
        return Fail(C,EFI_ACCESS_DENIED);
      }
    }
    if (Bytes==R->LiveBytes) { return Release(C,R); }
    if (Offset==R->First) { R->First+=Bytes; }
    R->LiveBytes-=Bytes; return EFI_SUCCESS;
  }
  return Fail(C,EFI_INVALID_PARAMETER);
}
STATIC EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *Io, EFI_PCI_IO_PROTOCOL_OPERATION Operation,
  VOID *Host, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Address, VOID **Mapping) {
  DALI_USB_XHCI_IO *C=Owner(Io);
  if (Host==NULL || Bytes==NULL || Address==NULL || Mapping==NULL || !*Bytes || *Bytes>MAX_BUFFER ||
      (UINTN)Host>MAX_UINTN-(*Bytes-1) ||
      (Operation!=EfiPciIoOperationBusMasterRead && Operation!=EfiPciIoOperationBusMasterWrite &&
       Operation!=EfiPciIoOperationBusMasterCommonBuffer)) { return Fail(C,EFI_INVALID_PARAMETER); }
  EFI_STATUS Status=Enter(C,FALSE); if (EFI_ERROR(Status)) { return Status; }
  RECORD *Parent=NULL;
  if (Operation==EfiPciIoOperationBusMasterCommonBuffer) {
    for (RECORD *R=C->Records; R!=NULL; R=R->Next) {
      if (R->Kind!=Allocation || (UINTN)Host<(UINTN)R->Buffer.Cpu) { continue; }
      UINTN Offset=(UINTN)Host-(UINTN)R->Buffer.Cpu;
      if (Offset>=R->First && Offset-R->First<=R->LiveBytes && *Bytes<=R->LiveBytes-(Offset-R->First)) { Parent=R; break; }
    }
    if (Parent==NULL) { return Fail(C,EFI_INVALID_PARAMETER); }
  }
  RECORD *R=New(C,Parent!=NULL ? CommonMap : Streaming); if (R==NULL) { return EFI_OUT_OF_RESOURCES; }
  R->Host=Host; R->Bytes=*Bytes; R->Parent=Parent;
  R->Direction=Operation==EfiPciIoOperationBusMasterRead ? DaliXhciToDevice : DaliXhciFromDevice;
  EFI_PHYSICAL_ADDRESS DeviceAddress=0;
  if (Parent!=NULL) {
    Parent->References++;
    UINTN Offset=(UINTN)Host-(UINTN)Parent->Buffer.Cpu;
    Status=Sync(C,Parent,Offset,*Bytes,DaliXhciBidirectional,TRUE);
    DeviceAddress=Parent->Buffer.Address+Offset;
  } else {
    Status=Allocate(C,R,ALIGN_VALUE(*Bytes,EFI_PAGE_SIZE),FALSE);
    if (!EFI_ERROR(Status)) {
      ZeroMem(R->Buffer.Cpu,R->Buffer.Size);
      if (R->Direction==DaliXhciToDevice) { CopyMem(R->Buffer.Cpu,Host,*Bytes); }
      Status=Sync(C,R,0,*Bytes,R->Direction,TRUE); DeviceAddress=R->Buffer.Address;
    }
  }
  if (!EFI_ERROR(Status)) { *Address=DeviceAddress; *Mapping=R; }
  return Status;
}
STATIC EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *Io, VOID *Mapping) {
  DALI_USB_XHCI_IO *C=Owner(Io);
  EFI_STATUS Status=Enter(C,TRUE); if (EFI_ERROR(Status)) { return Status; }
  RECORD *R=C->Records;
  while (R!=NULL && (R!=Mapping || R->Kind==Allocation)) { R=R->Next; }
  if (R==NULL) { return Fail(C,EFI_INVALID_PARAMETER); }
  if (R->Kind==CommonMap) {
    if (!C->Quiescing) { Status=Sync(C,R->Parent,(UINTN)R->Host-(UINTN)R->Parent->Buffer.Cpu,R->Bytes,DaliXhciBidirectional,FALSE); }
    if (EFI_ERROR(Status)) { return Status; }
    R->Parent->References--; Remove(C,R); return EFI_SUCCESS;
  }
  if (!C->Quiescing) {
    Status=Sync(C,R,0,R->Bytes,R->Direction,FALSE);
    if (EFI_ERROR(Status)) { return Status; }
    if (R->Direction==DaliXhciFromDevice) { CopyMem(R->Host,R->Buffer.Cpu,R->Bytes); }
  }
  return Release(C,R);
}
STATIC EFI_STATUS EFIAPI Flush(EFI_PCI_IO_PROTOCOL *Io) {
  DALI_USB_XHCI_IO *C=Owner(Io); EFI_STATUS Status=Enter(C,FALSE); UINT32 Value;
  if (EFI_ERROR(Status)) { return Status; }
  // Device read drains posted controller writes; streaming cache ownership is
  // explicitly returned by Unmap, and concurrent common buffers are coherent.
  return Read(C,C->OpOffset+4,4,&Value);
}
EFI_STATUS DaliUsbXhciIoOpen(DALI_USB_XHCI_IO *C, UINTN Base, UINTN Size,
  CONST DALI_USB_XHCI_DMA_OPS *Dma, DALI_USB_XHCI_IO_GUARD Guard, VOID *Context) {
  if (C==NULL || Dma==NULL || Guard==NULL || Dma->Allocate==NULL || Dma->Sync==NULL || Dma->Free==NULL ||
      Size<0x100 || (Base&3) || Base>MAX_UINTN-(Size-1)) { return EFI_INVALID_PARAMETER; }
  if (C->Closed) { return EFI_UNSUPPORTED; }
  if (C->Initialized) { return EFI_ALREADY_STARTED; }
  C->Initialized=TRUE; C->Owned=C->Faulted=C->Quiescing=C->HaltProven=FALSE; C->OpOffset=0;
  C->Base=Base; C->Size=Size; C->Dma=*Dma; C->Guard=Guard; C->GuardContext=Context;
  ZeroMem(&C->Io,sizeof C->Io);
  C->Io.Mem.Read=MemRead; C->Io.Mem.Write=MemWrite; C->Io.Pci.Read=PciRead;
  C->Io.AllocateBuffer=AllocateBuffer; C->Io.FreeBuffer=FreeBuffer;
  C->Io.Map=Map; C->Io.Unmap=Unmap; C->Io.Flush=Flush;
  if (!Dma->Qualified) { return Fail(C,EFI_UNSUPPORTED); }
  UINT8 Length=0; EFI_STATUS Status=Read(C,0,1,&Length);
  if (!EFI_ERROR(Status) && (Length<0x20 || (Length&3) || Length>Size-8)) { Status=Fail(C,EFI_UNSUPPORTED); }
  if (!EFI_ERROR(Status)) {
    C->OpOffset=Length;
    UINT32 Cmd,Sts; Status=Read(C,C->OpOffset,4,&Cmd);
    if (!EFI_ERROR(Status)) { Status=Read(C,C->OpOffset+4,4,&Sts); }
    if (!EFI_ERROR(Status) && ((Cmd&3) || !(Sts&1) || (Sts&0x800))) { Status=Fail(C,EFI_NOT_READY); }
    if (!EFI_ERROR(Status)) { C->Owned=C->HaltProven=TRUE; }
  }
  return Status;
}
EFI_STATUS DaliUsbXhciIoQuiesce(DALI_USB_XHCI_IO *C, UINT32 TimeoutUs) {
  if (C==NULL || !C->Initialized || !TimeoutUs || TimeoutUs>1000000) { return EFI_INVALID_PARAMETER; }
  if (C->Closed || !C->Owned || C->OpOffset<0x20 || C->OpOffset>C->Size-8) { return EFI_UNSUPPORTED; }
  C->Quiescing=TRUE; C->HaltProven=FALSE;
  UINT32 Cmd,Sts; EFI_STATUS Status=Read(C,C->OpOffset,4,&Cmd);
  if (EFI_ERROR(Status)) { return Status; }
  if (Cmd&2) { return Fail(C,EFI_NOT_READY); }
  Status=Proof(C,DaliXhciValidate); if (EFI_ERROR(Status)) { return Status; }
  MemoryFence(); MmioWrite32(C->Base+C->OpOffset,Cmd&~0x0dU); MemoryFence();
  if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  UINT32 Elapsed=0;
  for (;;) {
    Status=Read(C,C->OpOffset,4,&Cmd);
    if (!EFI_ERROR(Status)) { Status=Read(C,C->OpOffset+4,4,&Sts); }
    if (EFI_ERROR(Status)) { return Status; }
    if (!(Cmd&3) && (Sts&1) && !(Sts&0x800)) {
      // Fresh second hardware observation before authorizing any release.
      Status=Read(C,C->OpOffset,4,&Cmd);
      if (!EFI_ERROR(Status)) { Status=Read(C,C->OpOffset+4,4,&Sts); }
      if (EFI_ERROR(Status)) { return Status; }
      if (!(Cmd&3) && (Sts&1) && !(Sts&0x800)) { C->HaltProven=TRUE; return EFI_SUCCESS; }
    }
    if (Elapsed>=TimeoutUs) { return Fail(C,EFI_TIMEOUT); }
    UINT32 Delay=TimeoutUs-Elapsed<10 ? TimeoutUs-Elapsed : 10;
    MicroSecondDelay(Delay); Elapsed+=Delay;
    if (C->Closed) { return Fail(C,EFI_UNSUPPORTED); }
  }
}
EFI_STATUS DaliUsbXhciIoRearm(DALI_USB_XHCI_IO *C) {
  if (C==NULL || !C->Initialized || !C->Owned || !C->Quiescing || !C->HaltProven) { return EFI_INVALID_PARAMETER; }
  if (C->Closed) { return EFI_UNSUPPORTED; }
  if (C->Faulted) { return EFI_DEVICE_ERROR; }
  UINT32 Cmd=0,Sts=0;
  for (UINTN N=0; N<2; N++) {
    EFI_STATUS Status=Read(C,C->OpOffset,4,&Cmd);
    if (!EFI_ERROR(Status)) { Status=Read(C,C->OpOffset+4,4,&Sts); }
    if (EFI_ERROR(Status)) { return Status; }
    if ((Cmd&3) || !(Sts&1) || (Sts&0x804)) { return Fail(C,EFI_NOT_READY); }
  }
  C->Quiescing=FALSE; return EFI_SUCCESS;
}
EFI_STATUS DaliUsbXhciIoClose(DALI_USB_XHCI_IO *C) {
  if (C==NULL || !C->Initialized) { return EFI_INVALID_PARAMETER; }
  if (C->Closed) { return EFI_UNSUPPORTED; }
  EFI_STATUS Status=Proof(C,DaliXhciRelease); if (EFI_ERROR(Status)) { return Status; }
  if (C->Owned) {
    Status=DaliUsbXhciIoQuiesce(C,1000000); if (EFI_ERROR(Status)) { return Status; }
    RECORD *R=C->Records;
    while (R!=NULL) {
      RECORD *Next=R->Next;
      if (R->Kind==CommonMap) { R->Parent->References--; Remove(C,R); }
      R=Next;
    }
    while (C->Records!=NULL) { Status=Release(C,C->Records); if (EFI_ERROR(Status)) { return Status; } }
  }
  C->Initialized=C->Owned=FALSE; C->Guard=NULL; C->GuardContext=NULL; return EFI_SUCCESS;
}
