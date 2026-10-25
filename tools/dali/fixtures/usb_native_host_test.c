// Real lifecycle/HC protocol, source scheduler/register/memory/transfer objects.
#define main io_model_regressions
#include "usb_xhci_io_test.c"
#undef main
#include <stdarg.h>
#include <Library/DaliUsbNativeHostLib.h>
#include <Library/SynchronizationLib.h>
// Include the actual source unit to exercise its private descriptor validator.
#include "../../../Platforms/Xiaomi/daliPkg/Library/UsbNativeHostLib/MuEngine.c"
BOOLEAN XhcCheckUrbResult(USB_XHCI_INSTANCE *, URB *);
BOOLEAN DaliNativeXhciIgnoreSpuriousSuccess(USB_XHCI_INSTANCE *, UINT8, UINT8, UINT8);
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
  installs++;if(fail_install)return EFI_DEVICE_ERROR;*handle=&host;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE handle,...) {
  assert(handle==&host);uninstalls++;return fail_uninstall?EFI_ACCESS_DENIED:EFI_SUCCESS;
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
  assert(DaliUsbNativeHostPrepareHandoff(&host)==EFI_SUCCESS && host.Prepared && c.HaltProven);
  assert(m.pool_ops==pool_ops && m.frees==frees);
  assert(DaliUsbNativeHostPrepareHandoff(&host)==EFI_SUCCESS);
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
  for(unsigned n=0;n<=2;n+=2) {
    d->EndpointTransferRing[n]=AllocateZeroPool(sizeof(TRANSFER_RING));
    CreateTransferRing(x,TR_RING_TRB_NUMBER,d->EndpointTransferRing[n]);
  }
  ((DEVICE_CONTEXT *)d->OutputContext)->EP[2].EPType=ED_INTERRUPT_IN;
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
  lifecycle();failures();descriptors();async_lifecycle();malformed_completion();short_completion_spurious_success();ebs_latch();native_reset();reset();
  puts("PASS: actual native USB2 HC lifecycle, async remap/cancel, short-event quirk and malformed event-length fault; register model only");
  return 0;
}
