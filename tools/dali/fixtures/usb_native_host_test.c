// Real lifecycle/HC protocol, source scheduler/register/memory/transfer objects.
#ifndef DALI_USB_NATIVE_WORKER_TEST
#define main io_model_regressions
#include "usb_xhci_io_test.c"
#undef main
#endif
#include <stdarg.h>
#include <Library/DaliUsbNativeHostLib.h>
#include <Library/SynchronizationLib.h>
// Include the actual source unit to exercise its private descriptor validator.
#include "../../../Platforms/Xiaomi/daliPkg/Library/UsbNativeHostLib/MuEngine.c"
BOOLEAN XhcCheckUrbResult(USB_XHCI_INSTANCE *, URB *);
BOOLEAN DaliNativeXhciIgnoreSpuriousSuccess(USB_XHCI_INSTANCE *, UINT8, UINT8, UINT8);
UINT8 EFIAPI XhcInitializeEndpointContext(USB_XHCI_INSTANCE *, UINT8, UINT8, INPUT_CONTEXT *, USB_INTERFACE_DESCRIPTOR *);
UINT8 EFIAPI XhcInitializeEndpointContext64(USB_XHCI_INSTANCE *, UINT8, UINT8, INPUT_CONTEXT_64 *, USB_INTERFACE_DESCRIPTOR *);
EFI_STATUS DaliNativeXhciBeginEndpointConfiguration(USB_XHCI_INSTANCE *, UINT8, BOOLEAN, UINT8);
EFI_STATUS DaliNativeXhciCommitEndpointConfiguration(USB_XHCI_INSTANCE *, UINT8);
EFI_STATUS DaliNativeXhciRollbackEndpointConfiguration(USB_XHCI_INSTANCE *, UINT8);
EFI_STATUS DaliNativeXhciRemoveSlotReservations(USB_XHCI_INSTANCE *, UINT8);
static DALI_USB_NATIVE_HOST host;
static EFI_TPL tpl=TPL_APPLICATION;
static struct {
  EFI_EVENT_NOTIFY fn; VOID *context; BOOLEAN live; UINT32 type; EFI_TPL tpl;
} events[8];
static UINTN event_ops,fail_event,installs,uninstalls,disconnects;
static BOOLEAN fail_install,fail_uninstall;
static UINTN callbacks,short_callbacks;
static BOOLEAN cancel_in_callback;
static EFI_DEVICE_PATH_PROTOCOL path={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
EFI_GUID gEfiUsb2HcProtocolGuid={0},gEfiDevicePathProtocolGuid={1};
EFI_GUID gDaliUsbNativeHostHandoffProtocolGuid=DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL_GUID;
VOID EFIAPI DebugAssert(CONST CHAR8 *file,UINTN line,CONST CHAR8 *description) {
  fprintf(stderr,"DEBUG ASSERT %s:%llu %s\n",file,(unsigned long long)line,description);abort();
}
VOID EFIAPI DebugPrint(UINTN level,CONST CHAR8 *format,...) {(void)level;(void)format;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID) {return TRUE;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN level) {(void)level;return FALSE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID) {return TRUE;}
BOOLEAN EFIAPI DebugClearMemoryEnabled(VOID) {return FALSE;}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *v,UINT32 compare,UINT32 exchange) {
  return __sync_val_compare_and_swap(v,compare,exchange);
}
VOID *EFIAPI AllocateCopyPool(UINTN bytes,CONST VOID *data) {
  VOID *p=AllocatePool(bytes);return p?CopyMem(p,data,bytes):NULL;
}
INTN EFIAPI HighBitSet32(UINT32 value) {return value?31-__builtin_clz(value):-1;}
INTN EFIAPI HighBitSet64(UINT64 value) {return value?63-__builtin_clzll(value):-1;}
UINT64 EFIAPI LShiftU64(UINT64 value,UINTN shift) {assert(shift<64);return value<<shift;}
UINT64 EFIAPI RShiftU64(UINT64 value,UINTN shift) {assert(shift<64);return value>>shift;}
UINT64 EFIAPI MultU64x32(UINT64 value,UINT32 mul) {return value*mul;}
UINT64 EFIAPI MultU64x64(UINT64 value,UINT64 mul) {return value*mul;}
UINT64 EFIAPI DivU64x32(UINT64 value,UINT32 div) {return value/div;}
UINT64 EFIAPI DivU64x64Remainder(UINT64 value,UINT64 div,UINT64 *remainder) {if(remainder)*remainder=value%div;return value/div;}
UINT64 EFIAPI GetPerformanceCounter(VOID) {static UINT64 tick;return ++tick;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *start,UINT64 *end) {
  if(start) { *start=0; }
  if(end) { *end=~(UINT64)0; }
  return 1000000;
}
EFI_STATUS EFIAPI ReportStatusCodeWithDevicePath(EFI_STATUS_CODE_TYPE type,EFI_STATUS_CODE_VALUE value,
  CONST EFI_DEVICE_PATH_PROTOCOL *device) {(void)type;(void)value;(void)device;return TRUE;}
BOOLEAN EFIAPI ReportProgressCodeEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI ReportErrorCodeEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI ReportDebugCodeEnabled(VOID) {return FALSE;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL value) {assert(value>=tpl);EFI_TPL old=tpl;tpl=value;return old;}
static VOID EFIAPI restore_tpl(EFI_TPL value) {assert(value<=tpl);tpl=value;}
static EFI_STATUS EFIAPI stall(UINTN us) {MicroSecondDelay(us);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI create_event(UINT32 type,EFI_TPL value,EFI_EVENT_NOTIFY fn,VOID *context,EFI_EVENT *out) {
  if(++event_ops==fail_event)return EFI_OUT_OF_RESOURCES;
  for(unsigned n=0;n<8;n++)if(!events[n].live) {
    events[n].fn=fn;events[n].context=context;events[n].live=TRUE;events[n].type=type;events[n].tpl=value;*out=&events[n];return EFI_SUCCESS;
  }
  abort();
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT event) {
  for(unsigned n=0;n<8;n++) {
    if(event==&events[n]) {assert(events[n].live);events[n].live=FALSE;return EFI_SUCCESS;}
  }
  abort();
}
static EFI_STATUS EFIAPI timer(EFI_EVENT event,EFI_TIMER_DELAY delay,UINT64 trigger) {
  (void)event;assert(delay==TimerPeriodic && trigger==10000);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI install(EFI_HANDLE *handle,...) {
  installs++;if(fail_install)return EFI_DEVICE_ERROR;
  va_list args;va_start(args,handle);BOOLEAN hc=FALSE,path_present=FALSE,handoff=FALSE;EFI_GUID *guid;
  while((guid=va_arg(args,EFI_GUID *))!=NULL) {
    VOID *interface=va_arg(args,VOID *);
    if(guid==&gEfiUsb2HcProtocolGuid) {assert(interface==&host.Protocol);hc=TRUE;}
    else if(guid==&gEfiDevicePathProtocolGuid) {assert(interface==&path);path_present=TRUE;}
    else if(guid==&gDaliUsbNativeHostHandoffProtocolGuid) {
      assert(interface==&host.HandoffProtocol && host.HandoffProtocol.Revision==DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL_REVISION &&
        host.HandoffProtocol.PrepareHandoff!=NULL);handoff=TRUE;
    } else {assert(0);}
  }
  va_end(args);assert(hc && path_present && handoff);*handle=&host;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE handle,...) {
  assert(handle==&host);uninstalls++;
  va_list args;va_start(args,handle);BOOLEAN hc=FALSE,path_present=FALSE,handoff=FALSE;EFI_GUID *guid;
  while((guid=va_arg(args,EFI_GUID *))!=NULL) {
    VOID *interface=va_arg(args,VOID *);
    if(guid==&gEfiUsb2HcProtocolGuid) {assert(interface==&host.Protocol);hc=TRUE;}
    else if(guid==&gEfiDevicePathProtocolGuid) {assert(interface==&path);path_present=TRUE;}
    else if(guid==&gDaliUsbNativeHostHandoffProtocolGuid) {assert(interface==&host.HandoffProtocol);handoff=TRUE;}
    else {assert(0);}
  }
  va_end(args);assert(hc && path_present && handoff);return fail_uninstall?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI disconnect(EFI_HANDLE handle,EFI_HANDLE driver,EFI_HANDLE child) {
  assert(handle==&host && !driver && !child && tpl==TPL_APPLICATION);disconnects++;return EFI_SUCCESS;
}
static void native_reset(void) {
  reset();memset(&host,0,sizeof host);memset(events,0,sizeof events);
  event_ops=fail_event=installs=uninstalls=disconnects=0;fail_install=fail_uninstall=FALSE;tpl=TPL_APPLICATION;
  services=(EFI_BOOT_SERVICES){.FreePool=service_free,.RaiseTPL=raise_tpl,.RestoreTPL=restore_tpl,
    .Stall=stall,.CreateEvent=create_event,.CloseEvent=close_event,.SetTimer=timer,
    .InstallMultipleProtocolInterfaces=install,.UninstallMultipleProtocolInterfaces=uninstall,.DisconnectController=disconnect};
  m.hc_autoreset=TRUE;m.regs[0]=0x01000040;m.regs[1]=(2U<<24)|(1U<<8)|8;
  callbacks=short_callbacks=0;cancel_in_callback=FALSE;
  m.regs[2]=2U<<27;m.regs[4]=0x280U<<16;m.regs[5]=0x800;m.regs[6]=0x900;
  m.regs[0x48/4]=1;m.regs[0xa00/4]=0x02000002;m.regs[0xa04/4]=0x20425355;m.regs[0xa08/4]=0x0201;
}
static EFI_STATUS start(void) {UINT8 ports[]={1,0};assert(open_io()==EFI_SUCCESS);return DaliUsbNativeHostStart(&host,&c,&path,ports,2);}
static void finish(void) {
  assert(DaliUsbNativeHostStop(&host)==EFI_SUCCESS && !host.Engine && !host.Io && !host.Published);
  assert(c.Quiescing && c.HaltProven);close_io();
  for(unsigned n=0;n<8;n++)assert(!events[n].live);
  for(unsigned n=0;n<4096;n++)assert(!m.pool[n]);
}
static void lifecycle(void) {
  native_reset();assert(start()==EFI_SUCCESS && host.Published && host.Engine && (m.regs[0x40/4]&1));
  assert(installs==1 && m.allocs>=4);
  UINT8 speed=9,ports=9,dma=9;EFI_USB_HC_STATE state;
  assert(host.Protocol.GetCapability(&host.Protocol,&speed,&ports,&dma)==EFI_SUCCESS && speed==EFI_USB_SPEED_HIGH && ports==2 && !dma);
  assert(host.Protocol.SetState(&host.Protocol,EfiUsbHcStateHalt)==EFI_SUCCESS && c.HaltProven);
  assert(host.Protocol.GetState(&host.Protocol,&state)==EFI_SUCCESS && state==EfiUsbHcStateHalt);
  assert(host.Protocol.SetState(&host.Protocol,EfiUsbHcStateOperational)==EFI_SUCCESS && !c.Quiescing);
  UINTN allocations=m.allocs;
  assert(host.Protocol.Reset(&host.Protocol,EFI_USB_HC_RESET_GLOBAL)==EFI_SUCCESS && m.allocs>allocations);
  assert(host.Protocol.GetState(&host.Protocol,&state)==EFI_SUCCESS && state==EfiUsbHcStateHalt);
  assert(host.Protocol.SetState(&host.Protocol,EfiUsbHcStateOperational)==EFI_SUCCESS);
  UINTN writes=m.writes;
  assert(host.Protocol.GetRootHubPortStatus(&host.Protocol,2,(EFI_USB_PORT_STATUS *)&state)==EFI_INVALID_PARAMETER && m.writes==writes);
  UINTN pool_ops=m.pool_ops,frees=m.frees;
  assert(host.HandoffProtocol.Revision==DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL_REVISION);
  assert(host.HandoffProtocol.PrepareHandoff(&host.HandoffProtocol)==EFI_SUCCESS && host.Prepared && c.HaltProven);
  assert(m.pool_ops==pool_ops && m.frees==frees);
  assert(host.HandoffProtocol.PrepareHandoff(&host.HandoffProtocol)==EFI_SUCCESS);
  assert(host.Protocol.SetState(&host.Protocol,EfiUsbHcStateOperational)==EFI_DEVICE_ERROR);
  finish();assert(uninstalls==1 && disconnects==1);
}
static void failures(void) {
  for(unsigned n=0;n<6;n++) {
    native_reset();
    if(n==0)m.regs[0x48/4]=2;
    if(n==1)m.regs[5]=0xffc;
    if(n==2)m.regs[0xa00/4]=0x02000102; // overlapping next capability
    if(n==3)m.regs[0xa08/4]|=1U<<28;
    if(n==4)m.regs[0xa00/4]=0x01010001; // inherited BIOS
    if(n==5)m.regs[1]&=~255U;
    assert(EFI_ERROR(start()) && !host.Published && !m.allocs && host.Faulted);finish();
  }
  for(unsigned n=1;n<=4;n++) {
    native_reset();m.fail_allocate=n;
    assert(EFI_ERROR(start()) && host.Faulted && c.Records && !host.Published);
    m.fail_allocate=0;finish();
  }
  for(unsigned n=1;n<=2;n++) {
    native_reset();fail_event=n;assert(EFI_ERROR(start()) && host.Faulted && !host.Published);fail_event=0;finish();
  }
  native_reset();fail_install=TRUE;assert(start()==EFI_DEVICE_ERROR && host.Faulted);fail_install=FALSE;finish();
  native_reset();assert(start()==EFI_SUCCESS);m.halt_timeout=TRUE;UINTN frees=m.frees;
  assert(DaliUsbNativeHostPrepareHandoff(&host)==EFI_TIMEOUT && host.Engine && m.frees==frees && c.Records);
  assert(DaliUsbNativeHostStop(&host)==EFI_TIMEOUT && host.Published && m.frees==frees);
  m.halt_timeout=FALSE;finish();
  native_reset();assert(start()==EFI_SUCCESS);fail_uninstall=TRUE;
  assert(DaliUsbNativeHostStop(&host)==EFI_ACCESS_DENIED && host.Engine && host.Published && c.HaltProven);
  fail_uninstall=FALSE;finish();
  native_reset();assert(start()==EFI_SUCCESS);UINTN pools=m.pool_ops;finish();
  for(UINTN at=2;at<=pools;at++) {
    native_reset();m.fail_pool=at;assert(EFI_ERROR(start()) && !host.Published && host.Engine);
    m.fail_pool=0;finish();
  }
  printf("PASS: DEBUG source startup allocation failures at %llu pool operations and 4 provider allocations\n",(unsigned long long)(pools-1));
}
static void descriptors(void) {
  UINT8 valid[]={9,2,25,0,1,7,0,0x80,50, 9,4,42,0,1,3,1,1,0, 7,5,0x81,3,8,0,1};
  assert(XhcValidateConfigDescriptor(valid,sizeof valid));
  for(UINTN n=0;n<sizeof valid;n++)assert(!XhcValidateConfigDescriptor(valid,n));
  UINT8 bad[sizeof valid];
  const unsigned positions[]={0,1,4,5,9,10,13,18,20};
  const UINT8 values[]={0,3,2,0,0,5,2,0,0};
  for(unsigned n=0;n<sizeof positions/sizeof positions[0];n++) {
    memcpy(bad,valid,sizeof bad);bad[positions[n]]=values[n];assert(!XhcValidateConfigDescriptor(bad,sizeof bad));
  }
  USB_DEV_CONTEXT device={0};EFI_USB_CONFIG_DESCRIPTOR *configs[]={NULL,(VOID *)valid};
  device.DevDesc.NumConfigurations=2;device.ConfDesc=configs;
  assert(XhcCachedConfig(&device,7)==(VOID *)valid && !XhcCachedConfig(&device,1));
}
static void make_slot(void) {
  USB_XHCI_INSTANCE *x=host.Engine;USB_DEV_CONTEXT *d=&x->UsbDevContext[1];
  d->Enabled=TRUE;d->SlotId=1;d->BusDevAddr=1;
  d->InputContext=UsbHcAllocateMem(x->MemPool,sizeof(INPUT_CONTEXT),FALSE);
  d->OutputContext=UsbHcAllocateMem(x->MemPool,sizeof(DEVICE_CONTEXT),FALSE);
  ((DEVICE_CONTEXT *)d->OutputContext)->Slot.Speed=EFI_USB_SPEED_HIGH;
  ((DEVICE_CONTEXT *)d->OutputContext)->Slot.RootHubPortNum=1;
  for(unsigned n=0;n<=2;n+=2) {
    d->EndpointTransferRing[n]=AllocateZeroPool(sizeof(TRANSFER_RING));
    CreateTransferRing(x,TR_RING_TRB_NUMBER,d->EndpointTransferRing[n]);
  }
  ((DEVICE_CONTEXT *)d->OutputContext)->EP[2].EPType=ED_INTERRUPT_IN;
}
static void make_sched_device(UINT8 slot,UINT8 speed,UINT8 tt_hub,UINT8 tt_port) {
  USB_XHCI_INSTANCE *x=host.Engine;USB_DEV_CONTEXT *d=&x->UsbDevContext[slot];
  d->Enabled=TRUE;d->SlotId=slot;
  d->InputContext=UsbHcAllocateMem(x->MemPool,sizeof(INPUT_CONTEXT),FALSE);
  d->OutputContext=UsbHcAllocateMem(x->MemPool,sizeof(DEVICE_CONTEXT),FALSE);
  DEVICE_CONTEXT *out=d->OutputContext;
  out->Slot.Speed=(UINT8)(speed+1U);out->Slot.RootHubPortNum=1;
  out->Slot.TTHubSlotId=tt_hub;out->Slot.TTPortNum=tt_port;
}
static void make_sched_device64(UINT8 slot,UINT8 speed) {
  USB_XHCI_INSTANCE *x=host.Engine;USB_DEV_CONTEXT *d=&x->UsbDevContext[slot];
  d->Enabled=TRUE;d->SlotId=slot;
  d->InputContext=UsbHcAllocateMem(x->MemPool,sizeof(INPUT_CONTEXT_64),FALSE);
  d->OutputContext=UsbHcAllocateMem(x->MemPool,sizeof(DEVICE_CONTEXT_64),FALSE);
  DEVICE_CONTEXT_64 *out=d->OutputContext;
  out->Slot.Speed=(UINT8)(speed+1U);out->Slot.RootHubPortNum=1;
}
static USB_INTERFACE_DESCRIPTOR *sched_interface(UINT8 *bytes,UINT8 count,UINT8 first_address,
                                                  UINT16 max_packet,UINT8 interval) {
  USB_INTERFACE_DESCRIPTOR *iface=(VOID *)bytes;
  ZeroMem(bytes,sizeof(USB_INTERFACE_DESCRIPTOR)+(UINTN)count*sizeof(USB_ENDPOINT_DESCRIPTOR));
  iface->Length=sizeof(*iface);iface->DescriptorType=USB_DESC_TYPE_INTERFACE;
  iface->InterfaceNumber=0;iface->AlternateSetting=0;iface->NumEndpoints=count;
  USB_ENDPOINT_DESCRIPTOR *ep=(VOID *)(bytes+iface->Length);
  for(UINTN n=0;n<count;n++,ep++) {
    ep->Length=sizeof(*ep);ep->DescriptorType=USB_DESC_TYPE_ENDPOINT;
    if(count==1) {ep->EndpointAddress=first_address;}
    else if(n<15) {ep->EndpointAddress=(UINT8)(0x81+n);}
    else {ep->EndpointAddress=1;}
    ep->Attributes=USB_ENDPOINT_INTERRUPT;ep->MaxPacketSize=max_packet;ep->Interval=interval;
  }
  return iface;
}
static UINT8 init_sched_interface(UINT8 slot,UINT8 speed,USB_INTERFACE_DESCRIPTOR *iface) {
  USB_XHCI_INSTANCE *x=host.Engine;USB_DEV_CONTEXT *d=&x->UsbDevContext[slot];
  INPUT_CONTEXT *input=d->InputContext;DEVICE_CONTEXT *output=d->OutputContext;
  ZeroMem(input,sizeof(*input));CopyMem(&input->Slot,&output->Slot,sizeof(input->Slot));
  return XhcInitializeEndpointContext(x,slot,speed,input,iface);
}
static UINT8 init_sched_interface64(UINT8 slot,UINT8 speed,USB_INTERFACE_DESCRIPTOR *iface) {
  USB_XHCI_INSTANCE *x=host.Engine;USB_DEV_CONTEXT *d=&x->UsbDevContext[slot];
  INPUT_CONTEXT_64 *input=d->InputContext;DEVICE_CONTEXT_64 *output=d->OutputContext;
  ZeroMem(input,sizeof(*input));CopyMem(&input->Slot,&output->Slot,sizeof(input->Slot));
  return XhcInitializeEndpointContext64(x,slot,speed,input,iface);
}
static void mtk_high_speed_scheduler(void) {
  native_reset();assert(start()==EFI_SUCCESS);make_slot();
  USB_XHCI_INSTANCE *x=host.Engine;
  UINT8 bytes[sizeof(USB_INTERFACE_DESCRIPTOR)+3*sizeof(USB_ENDPOINT_DESCRIPTOR)];
  USB_INTERFACE_DESCRIPTOR *iface=sched_interface(bytes,1,0x81,(UINT16)(1024|(2U<<11)),1);
  assert(DaliNativeXhciBeginEndpointConfiguration(x,1,TRUE,0)==EFI_SUCCESS);
  UINT8 dci=init_sched_interface(1,EFI_USB_SPEED_HIGH,iface);assert(dci==3);
  ENDPOINT_CONTEXT *ep=&((INPUT_CONTEXT *)x->UsbDevContext[1].InputContext)->EP[dci-1];
  assert(ep->MaxPacketSize==1024 && ep->MaxBurstSize==2 && ep->Mult==0 && ep->MaxESITPayload==3072);
  assert(ep->RsvdZ5==3 && ep->RsvdZ6==0 && ep->RsvdZ7==0);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,1)==EFI_SUCCESS);

  assert(DaliNativeXhciBeginEndpointConfiguration(x,1,TRUE,0)==EFI_SUCCESS);
  iface=sched_interface(bytes,3,0x81,(UINT16)(1024|(2U<<11)),1);
  assert(init_sched_interface(1,EFI_USB_SPEED_HIGH,iface)==0);
  assert(DaliNativeXhciRollbackEndpointConfiguration(x,1)==EFI_SUCCESS && !host.Faulted);

  assert(DaliNativeXhciBeginEndpointConfiguration(x,1,FALSE,1)==EFI_SUCCESS);
  iface=sched_interface(bytes,1,0x82,(UINT16)(1024|(2U<<11)),1);
  iface->InterfaceNumber=1;iface->AlternateSetting=1;
  assert(init_sched_interface(1,EFI_USB_SPEED_HIGH,iface)==5);
  iface=sched_interface(bytes,1,0x83,(UINT16)(1024|(2U<<11)),1);
  iface->InterfaceNumber=1;iface->AlternateSetting=1;
  assert(init_sched_interface(1,EFI_USB_SPEED_HIGH,iface)==0);
  assert(DaliNativeXhciRollbackEndpointConfiguration(x,1)==EFI_SUCCESS && !host.Faulted);

  assert(DaliNativeXhciBeginEndpointConfiguration(x,1,FALSE,1)==EFI_SUCCESS);
  iface=sched_interface(bytes,1,0x82,(UINT16)(1024|(2U<<11)),1);
  iface->InterfaceNumber=1;iface->AlternateSetting=1;
  assert(init_sched_interface(1,EFI_USB_SPEED_HIGH,iface)==5);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,1)==EFI_SUCCESS);

  make_sched_device(3,EFI_USB_SPEED_HIGH,0,0);
  ((DEVICE_CONTEXT *)x->UsbDevContext[3].OutputContext)->Slot.RootHubPortNum=2;
  assert(DaliNativeXhciBeginEndpointConfiguration(x,3,TRUE,0)==EFI_SUCCESS);
  iface=sched_interface(bytes,1,0x81,(UINT16)(1024|(2U<<11)),1);
  assert(init_sched_interface(3,EFI_USB_SPEED_HIGH,iface)==3);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,3)==EFI_SUCCESS);
  assert(DaliNativeXhciRemoveSlotReservations(x,3)==EFI_SUCCESS);

  make_sched_device(2,EFI_USB_SPEED_HIGH,0,0);
  assert(DaliNativeXhciBeginEndpointConfiguration(x,2,TRUE,0)==EFI_SUCCESS);
  iface=sched_interface(bytes,1,0x81,(UINT16)(1024|(2U<<11)),1);
  assert(init_sched_interface(2,EFI_USB_SPEED_HIGH,iface)==0);
  assert(DaliNativeXhciRollbackEndpointConfiguration(x,2)==EFI_SUCCESS);
  assert(DaliNativeXhciRemoveSlotReservations(x,1)==EFI_SUCCESS);
  assert(DaliNativeXhciBeginEndpointConfiguration(x,2,TRUE,0)==EFI_SUCCESS);
  assert(init_sched_interface(2,EFI_USB_SPEED_HIGH,iface)==3);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,2)==EFI_SUCCESS);
  assert(DaliNativeXhciRemoveSlotReservations(x,2)==EFI_SUCCESS);

  assert(DaliNativeXhciBeginEndpointConfiguration(x,1,TRUE,0)==EFI_SUCCESS);
  iface=sched_interface(bytes,1,0x81,64,9);
  assert(init_sched_interface(1,EFI_USB_SPEED_HIGH,iface)==3);
  ENDPOINT_CONTEXT *long_interval=&((INPUT_CONTEXT *)x->UsbDevContext[1].InputContext)->EP[2];
  assert(long_interval->Interval==8 && long_interval->MaxPacketSize==64);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,1)==EFI_SUCCESS);
  assert(DaliNativeXhciRemoveSlotReservations(x,1)==EFI_SUCCESS);
  finish();
}
static void mtk_high_speed_scheduler64(void) {
  native_reset();m.regs[4]|=1U<<2;assert(start()==EFI_SUCCESS);make_sched_device64(1,EFI_USB_SPEED_HIGH);
  USB_XHCI_INSTANCE *x=host.Engine;UINT8 bytes[sizeof(USB_INTERFACE_DESCRIPTOR)+sizeof(USB_ENDPOINT_DESCRIPTOR)];
  USB_INTERFACE_DESCRIPTOR *iface=sched_interface(bytes,1,0x81,(UINT16)(1024|(2U<<11)),1);
  assert(DaliNativeXhciBeginEndpointConfiguration(x,1,TRUE,0)==EFI_SUCCESS);
  assert(init_sched_interface64(1,EFI_USB_SPEED_HIGH,iface)==3);
  ENDPOINT_CONTEXT_64 *ep=&((INPUT_CONTEXT_64 *)x->UsbDevContext[1].InputContext)->EP[2];
  assert(ep->MaxPacketSize==1024 && ep->MaxBurstSize==2 && ep->MaxESITPayload==3072 && ep->RsvdZ5==3);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,1)==EFI_SUCCESS);
  assert(DaliNativeXhciRemoveSlotReservations(x,1)==EFI_SUCCESS);

  make_sched_device64(2,EFI_USB_SPEED_HIGH);
  SLOT_CONTEXT_64 *hub=&((DEVICE_CONTEXT_64 *)x->UsbDevContext[2].OutputContext)->Slot;
  hub->Hub=TRUE;hub->PortNum=4;hub->MTT=TRUE;
  make_sched_device64(3,EFI_USB_SPEED_FULL);
  SLOT_CONTEXT_64 *child=&((DEVICE_CONTEXT_64 *)x->UsbDevContext[3].OutputContext)->Slot;
  child->TTHubSlotId=2;child->TTPortNum=1;
  UINT8 tt_bytes[sizeof(USB_INTERFACE_DESCRIPTOR)+sizeof(USB_ENDPOINT_DESCRIPTOR)];
  USB_INTERFACE_DESCRIPTOR *tt_iface=sched_interface(tt_bytes,1,0x81,64,1);
  assert(DaliNativeXhciBeginEndpointConfiguration(x,3,TRUE,0)==EFI_SUCCESS);
  assert(init_sched_interface64(3,EFI_USB_SPEED_FULL,tt_iface)==3);
  ENDPOINT_CONTEXT_64 *tt_ep=&((INPUT_CONTEXT_64 *)x->UsbDevContext[3].InputContext)->EP[2];
  assert(tt_ep->MaxPacketSize==64 && tt_ep->RsvdZ5==0x101 && tt_ep->RsvdZ6==0);
  assert(DaliNativeXhciCommitEndpointConfiguration(x,3)==EFI_SUCCESS);
  assert(DaliNativeXhciRemoveSlotReservations(x,3)==EFI_SUCCESS);
  finish();
}
static void mtk_tt_scheduler(void) {
  for(UINTN multi=0;multi<2;multi++) {
    native_reset();assert(start()==EFI_SUCCESS);make_sched_device(2,EFI_USB_SPEED_HIGH,0,0);
    USB_XHCI_INSTANCE *x=host.Engine;
    DEVICE_CONTEXT *hub=x->UsbDevContext[2].OutputContext;
    hub->Slot.Speed=(UINT8)(EFI_USB_SPEED_HIGH+1U);
    hub->Slot.Hub=TRUE;hub->Slot.PortNum=8;
    hub->Slot.MTT=(UINT8)multi; // models the selected alt-1 multi-TT mode.
    make_sched_device(3,EFI_USB_SPEED_FULL,2,1);
    UINT8 bytes[sizeof(USB_INTERFACE_DESCRIPTOR)+16*sizeof(USB_ENDPOINT_DESCRIPTOR)];
    USB_INTERFACE_DESCRIPTOR *iface=sched_interface(bytes,14,0x81,64,1);
    assert(DaliNativeXhciBeginEndpointConfiguration(x,3,TRUE,0)==EFI_SUCCESS);
    assert(init_sched_interface(3,EFI_USB_SPEED_FULL,iface)==29);
    INPUT_CONTEXT *input=x->UsbDevContext[3].InputContext;
    ENDPOINT_CONTEXT *first=&input->EP[2];
    ENDPOINT_CONTEXT *last=&input->EP[28];
    assert(first->MaxPacketSize==64 && first->MaxESITPayload==64 && first->RsvdZ5==0x101);
    assert((last->RsvdZ6&0x3fffU)==7); // start-split Y6 is skipped; Y7 is used.
    assert(DaliNativeXhciCommitEndpointConfiguration(x,3)==EFI_SUCCESS);

    make_sched_device(4,EFI_USB_SPEED_FULL,2,2);
    UINT8 one[sizeof(USB_INTERFACE_DESCRIPTOR)+sizeof(USB_ENDPOINT_DESCRIPTOR)];
    iface=sched_interface(one,1,0x81,64,1);
    assert(DaliNativeXhciBeginEndpointConfiguration(x,4,TRUE,0)==EFI_SUCCESS);
    UINT8 result=init_sched_interface(4,EFI_USB_SPEED_FULL,iface);
    if(multi) {
      assert(result==3);
      ENDPOINT_CONTEXT *other=&((INPUT_CONTEXT *)x->UsbDevContext[4].InputContext)->EP[2];
      assert(other->RsvdZ5==0x101 && other->RsvdZ6==0);
      assert(DaliNativeXhciCommitEndpointConfiguration(x,4)==EFI_SUCCESS);
      assert(DaliNativeXhciRemoveSlotReservations(x,4)==EFI_SUCCESS);
    } else {
      assert(result==0);
      assert(DaliNativeXhciRollbackEndpointConfiguration(x,4)==EFI_SUCCESS);
      assert(DaliNativeXhciRemoveSlotReservations(x,3)==EFI_SUCCESS);
      assert(DaliNativeXhciBeginEndpointConfiguration(x,4,TRUE,0)==EFI_SUCCESS);
      assert(init_sched_interface(4,EFI_USB_SPEED_FULL,iface)==3);
      assert(DaliNativeXhciCommitEndpointConfiguration(x,4)==EFI_SUCCESS);
      assert(DaliNativeXhciRemoveSlotReservations(x,4)==EFI_SUCCESS);
    }
    finish();
  }
}
static EFI_STATUS EFIAPI callback(VOID *data,UINTN bytes,VOID *context,UINT32 result) {
  assert(context==&host && bytes==8 && !result && data && host.CallbackDepth==1 && !host.Busy);callbacks++;
  for(unsigned n=0;n<8;n++)assert(((UINT8 *)data)[n]==0x5a);
  assert(DaliUsbNativeHostStop(&host)==EFI_NOT_READY);
  if(cancel_in_callback) {
    USB_XHCI_INSTANCE *x=host.Engine; // model a completed Disable Slot command
    x->UsbDevContext[1].Enabled=FALSE;x->UsbDevContext[1].SlotId=0;
    UINT8 toggle=0;
    assert(host.Protocol.AsyncInterruptTransfer(&host.Protocol,1,0x81,0,0,FALSE,&toggle,0,0,NULL,NULL,NULL)==EFI_SUCCESS);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI short_callback(VOID *data,UINTN bytes,VOID *context,UINT32 result) {
  assert(context==&host && bytes==5 && !result && data && host.CallbackDepth==1 && !host.Busy);
  for(unsigned n=0;n<5;n++)assert(((UINT8 *)data)[n]==0x5a);
  short_callbacks++;
  return EFI_SUCCESS;
}
static URB *submit_async_callback(EFI_ASYNC_USB_TRANSFER_CALLBACK transfer_callback) {
  UINT8 toggle=0;assert(host.Protocol.AsyncInterruptTransfer(&host.Protocol,1,0x81,EFI_USB_SPEED_FULL,8,
    TRUE,&toggle,10,8,NULL,transfer_callback,&host)==EFI_SUCCESS);
  USB_XHCI_INSTANCE *x=host.Engine;assert(!IsListEmpty(&x->AsyncIntTransfers));
  return BASE_CR(GetFirstNode(&x->AsyncIntTransfers),URB,UrbList);
}
static URB *submit_async(void) {return submit_async_callback(callback);}
static void complete_async(URB *u) {
  USB_XHCI_INSTANCE *x=host.Engine;
  for(unsigned n=0;n<64;n++)if(m.dma[n].live && m.dma[n].address==(UINT64)(UINTN)u->DataPhy)memset(m.dma[n].cpu,0x5a,8);
  EFI_PHYSICAL_ADDRESS address=UsbHcGetPciAddrForHostAddr(x->MemPool,u->TrbEnd,sizeof(TRB_TEMPLATE),FALSE);
  EVT_TRB_TRANSFER *event=(VOID *)x->EventRing.EventRingDequeue;
  *event=(EVT_TRB_TRANSFER){.TRBPtrLo=(UINT32)address,.TRBPtrHi=(UINT32)(address>>32),.Length=0,
    .Completecode=TRB_COMPLETION_SUCCESS,.CycleBit=x->EventRing.EventRingCCS,.Type=TRB_TYPE_TRANS_EVENT,.EndpointId=3,.SlotId=1};
}
static void complete_async_short(URB *u) {
  USB_XHCI_INSTANCE *x=host.Engine;
  for(unsigned n=0;n<64;n++)if(m.dma[n].live && m.dma[n].address==(UINT64)(UINTN)u->DataPhy)memset(m.dma[n].cpu,0x5a,5);
  EFI_PHYSICAL_ADDRESS address=UsbHcGetPciAddrForHostAddr(x->MemPool,u->TrbEnd,sizeof(TRB_TEMPLATE),FALSE);
  EVT_TRB_TRANSFER *event=(VOID *)x->EventRing.EventRingDequeue;
  *event=(EVT_TRB_TRANSFER){.TRBPtrLo=(UINT32)address,.TRBPtrHi=(UINT32)(address>>32),.Length=3,
    .Completecode=TRB_COMPLETION_SHORT_PACKET,.CycleBit=x->EventRing.EventRingCCS,.Type=TRB_TYPE_TRANS_EVENT,.EndpointId=3,.SlotId=1};
}
static void spurious_success(void) {
  USB_XHCI_INSTANCE *x=host.Engine;
  EFI_PHYSICAL_ADDRESS address=UsbHcGetPciAddrForHostAddr(x->MemPool,x->CmdRing.RingSeg0,sizeof(TRB_TEMPLATE),FALSE);
  EVT_TRB_TRANSFER *event=(VOID *)x->EventRing.EventRingDequeue;
  *event=(EVT_TRB_TRANSFER){.TRBPtrLo=(UINT32)address,.TRBPtrHi=(UINT32)(address>>32),.Length=0,
    .Completecode=TRB_COMPLETION_SUCCESS,.CycleBit=x->EventRing.EventRingCCS,.Type=TRB_TYPE_TRANS_EVENT,.EndpointId=3,.SlotId=1};
}
static void async_lifecycle(void) {
  native_reset();assert(start()==EFI_SUCCESS);make_slot();URB *u=submit_async();complete_async(u);
  cancel_in_callback=TRUE;UINTN bells=m.doorbells;
  assert(DaliUsbNativeHostPoll(&host)==EFI_SUCCESS && callbacks==1 && m.doorbells==bells+1 && !host.CallbackDepth);
  assert(IsListEmpty(&((USB_XHCI_INSTANCE *)host.Engine)->AsyncIntTransfers));finish();
  native_reset();assert(start()==EFI_SUCCESS);make_slot();u=submit_async();complete_async(u);
  bells=m.doorbells;m.fail_sync=m.syncs+1;
  assert(DaliUsbNativeHostPoll(&host)==EFI_DEVICE_ERROR && !callbacks && host.Faulted && m.doorbells==bells && c.HaltProven);
  m.fail_sync=0;finish();
  native_reset();assert(start()==EFI_SUCCESS);make_slot();u=submit_async();
  // Cancellation command never completes: the native command hook must stop
  // DMA and retain the URB instead of using source's ignore-and-free path.
  UINT8 toggle=0;UINTN frees=m.frees;
  assert(EFI_ERROR(host.Protocol.AsyncInterruptTransfer(&host.Protocol,1,0x81,0,0,FALSE,&toggle,0,0,NULL,NULL,NULL)));
  assert(host.Faulted && c.HaltProven && m.frees==frees && !IsListEmpty(&((USB_XHCI_INSTANCE *)host.Engine)->AsyncIntTransfers));
  finish();
  puts("PASS: actual async completion/cache/remap/rearm and callback cancellation; failed remap/cancel stop before callback/rearm/free");
}
static void malformed_completion(void) {
  native_reset();assert(start()==EFI_SUCCESS);make_slot();URB *u=submit_async();complete_async(u);
  USB_XHCI_INSTANCE *x=host.Engine;
  ((EVT_TRB_TRANSFER *)x->EventRing.EventRingDequeue)->Length=9; // submitted TRB is only8 bytes
  UINTN frees=m.frees;
  XhcCheckUrbResult(x,u);
  assert(u->Finished && (u->Result&EFI_USB_ERR_BUFFER) && !u->Completed);
  assert(host.Faulted && c.HaltProven && m.frees==frees && !callbacks);
  finish();
}
static void short_completion_spurious_success(void) {
  native_reset();assert(start()==EFI_SUCCESS);make_slot();URB *u=submit_async_callback(short_callback);
  complete_async_short(u);
  assert(DaliUsbNativeHostPoll(&host)==EFI_SUCCESS && short_callbacks==1 && !callbacks);
  assert(!u->Finished && !u->DaliShortSeen);
  assert(DaliNativeXhciIgnoreSpuriousSuccess(host.Engine,1,3,TRB_COMPLETION_SUCCESS));
  complete_async_short(u);
  assert(DaliUsbNativeHostPoll(&host)==EFI_SUCCESS && short_callbacks==2 && !callbacks);
  spurious_success();
  assert(DaliUsbNativeHostPoll(&host)==EFI_SUCCESS && short_callbacks==2 && !callbacks);
  assert(!DaliNativeXhciIgnoreSpuriousSuccess(host.Engine,1,3,TRB_COMPLETION_SUCCESS));
  finish();
}
static void ebs_latch(void) {
  native_reset();assert(start()==EFI_SUCCESS);UINTN frees=m.frees;
  for(unsigned n=0;n<8;n++)if(events[n].live && events[n].type==EVT_SIGNAL_EXIT_BOOT_SERVICES) {
    EFI_TPL old=raise_tpl(events[n].tpl);events[n].fn(&events[n],events[n].context);restore_tpl(old);
  }
  assert(host.Closed && c.Closed && c.HaltProven && m.frees==frees && host.Engine && c.Records);
  UINTN ops=m.ops;UINT8 a,b,d;
  assert(host.Protocol.GetCapability(&host.Protocol,&a,&b,&d)==EFI_UNSUPPORTED);
  assert(DaliUsbNativeHostStop(&host)==EFI_UNSUPPORTED && DaliUsbNativeHostPrepareHandoff(&host)==EFI_UNSUPPORTED && m.ops==ops);
}
int main(void) {
  lifecycle();failures();descriptors();mtk_high_speed_scheduler();mtk_high_speed_scheduler64();mtk_tt_scheduler();async_lifecycle();malformed_completion();short_completion_spurious_success();ebs_latch();native_reset();reset();
  puts("PASS: actual native USB2 HC lifecycle, MTK HS/TT periodic admission and rollback, async remap/cancel and completion quirks; register model only");
  return 0;
}
