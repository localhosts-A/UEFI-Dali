// Production context included to exercise its private failure/lifetime state.
// GCD/ARM mappings are a model; FDT validation uses real pinned libfdt.
#include <Uefi.h>
#include <Library/BaseLib.h>
#define main ClockGraphMain
#include "usb_clock_fdt_test.c"
#undef main
#include "usb_vusb_fdt_fixture.h"
#include "usb_i2c_fdt_fixture.h"
#include "../../../Platforms/Xiaomi/daliPkg/Drivers/UsbResourceContextDxe/UsbResourceContextDxe.c"
#include <stdarg.h>

EFI_BOOT_SERVICES *gBS;
EFI_DXE_SERVICES *gDS;
EFI_GUID gDaliUsbResourceContextProtocolGuid = {.Data1=1};
EFI_GUID gEfiEventExitBootServicesGuid = {.Data1=2};
static EFI_BOOT_SERVICES bs;
static EFI_DXE_SERVICES ds;
static EFI_GCD_MEMORY_SPACE_DESCRIPTOR desc[10];
static const UINT64 pages[] = {0x1c016000,0x10000000,0x10001000,0x131b0000,0x16640000,0x1002d000,0x13110000,0x13170000,0x16410000,0x16700000};
static BOOLEAN cpu_mapped[10];
static BOOLEAN cpu_cached[10], gcd_sync_shortcut;
static UINTN operation, fail_at, arm_calls, freed, event_calls, install_calls, cache_flushes;
static BOOLEAN applied_error, no_pool, no_event, no_publish, mutate_copy, bad_address, bad_final;
static EFI_TPL current_tpl;
static EFI_EVENT_NOTIFY ebs;
static VOID *ebs_context;
static DALI_USB_RESOURCE_CONTEXT_PROTOCOL *published;
static EFI_HANDLE image = (EFI_HANDLE)(UINTN)0x1234;
static void reset(void);
static void entry(EFI_STATUS expected);
static UINT32 pin_mmio_read(UINTN);
static void pin_reset(void);
static BOOLEAN native_bus_enabled(void);
static UINT32 native_bus_read32(UINTN);
static UINT32 native_bus_write32(UINTN,UINT32);
static void native_bus_delay(UINTN);
static void native_bus_reset(void);
#include "usb_resource_power_fixture.h"
#include "usb_resource_pin_fixture.h"
#include "usb_resource_bus_fixture.h"

static UINTN index_of(UINT64 page) {
  for (UINTN i=0;i<10;i++) { if (pages[i]==page) { return i; } }
  assert(0); return 0;
}
static BOOLEAN before(void) { operation++; return operation==fail_at && !applied_error; }
static EFI_STATUS after(void) { return operation==fail_at ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
VOID *EFIAPI AllocatePool(UINTN size) { return no_pool ? NULL : malloc(size); }
VOID EFIAPI FreePool(VOID *value) { freed++; free(value); }
VOID *EFIAPI CopyMem(VOID *to,CONST VOID *from,UINTN size) {
  memcpy(to,from,size);
  if (mutate_copy && from==tree) { tree[sizeof tree-1]^=1; }
  return to;
}
VOID *EFIAPI ZeroMem(VOID *value,UINTN size) { return memset(value,0,size); }
INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN size) { return memcmp(a,b,size); }
static EFI_TPL EFIAPI raise_tpl(EFI_TPL tpl) { EFI_TPL old=current_tpl; current_tpl=tpl; return old; }
static VOID EFIAPI restore_tpl(EFI_TPL tpl) { current_tpl=tpl; power_on_restore(); }
static EFI_STATUS EFIAPI get_desc(EFI_PHYSICAL_ADDRESS page,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *out) {
  if (before()) { return EFI_DEVICE_ERROR; }
  *out=desc[index_of(page)];
  if (bad_final && out->ImageHandle==image) { out->ImageHandle=NULL; }
  return after();
}
static EFI_STATUS EFIAPI add_space(EFI_GCD_MEMORY_TYPE type,EFI_PHYSICAL_ADDRESS page,UINT64 size,UINT64 caps) {
  assert(type==EfiGcdMemoryTypeMemoryMappedIo && size==4096 && caps==EFI_MEMORY_UC);
  if (before()) { return EFI_DEVICE_ERROR; }
  UINTN i=index_of(page); assert(desc[i].GcdMemoryType==EfiGcdMemoryTypeNonExistent);
  desc[i].BaseAddress=page; desc[i].Length=size; desc[i].Capabilities=caps; desc[i].GcdMemoryType=type;
  return after();
}
static EFI_STATUS EFIAPI allocate_space(EFI_GCD_ALLOCATE_TYPE kind,EFI_GCD_MEMORY_TYPE type,
  UINTN alignment,UINT64 size,EFI_PHYSICAL_ADDRESS *page,EFI_HANDLE owner,EFI_HANDLE device) {
  assert(kind==EfiGcdAllocateAddress && type==EfiGcdMemoryTypeMemoryMappedIo && alignment==12 &&
    size==4096 && owner==image && device==NULL);
  if (before()) { return EFI_DEVICE_ERROR; }
  UINTN i=index_of(*page); assert(desc[i].ImageHandle==NULL && desc[i].DeviceHandle==NULL);
  desc[i].ImageHandle=owner; if (bad_address) { *page+=4096; }
  return after();
}
RETURN_STATUS EFIAPI ArmSetMemoryAttributes(EFI_PHYSICAL_ADDRESS page,UINT64 size,UINT64 attrs,UINT64 mask) {
  assert(size==4096 && (attrs==EFI_MEMORY_UC || attrs==(EFI_MEMORY_UC|EFI_MEMORY_XP)) && mask==0);
  UINTN i=index_of(page); assert(desc[i].ImageHandle==image);
  assert(!cpu_cached[i]); // CpuDxe must see/cache-maintain old WB before direct MMU.
  arm_calls++;
  if (before()) { return EFI_DEVICE_ERROR; }
  cpu_mapped[i]=TRUE; return after();
}
static EFI_STATUS EFIAPI set_attrs(EFI_PHYSICAL_ADDRESS page,UINT64 size,UINT64 attrs) {
  UINTN i=index_of(page); assert(size==4096 && desc[i].ImageHandle==image);
  if (before()) { return EFI_DEVICE_ERROR; }
  if (!gcd_sync_shortcut) {
    if (cpu_cached[i]) { cache_flushes++; cpu_cached[i]=FALSE; }
    cpu_mapped[i]=TRUE;
  }
  desc[i].Attributes=attrs; return after();
}
static EFI_STATUS EFIAPI create_event(UINT32 type,EFI_TPL tpl,EFI_EVENT_NOTIFY notify,CONST VOID *context,
  CONST EFI_GUID *group,EFI_EVENT *out) {
  event_calls++; assert(current_tpl==TPL_APPLICATION && type==EVT_NOTIFY_SIGNAL && tpl==TPL_NOTIFY &&
    group==&gEfiEventExitBootServicesGuid);
  if (no_event) { return EFI_OUT_OF_RESOURCES; }
  ebs=notify; ebs_context=(VOID *)context; *out=(EFI_EVENT)(UINTN)1; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI publish(EFI_HANDLE *handle,...) {
  install_calls++; assert(current_tpl==TPL_APPLICATION);
  va_list args; va_start(args,handle);
  assert(va_arg(args,EFI_GUID *)==&gDaliUsbResourceContextProtocolGuid);
  DALI_USB_RESOURCE_CONTEXT_PROTOCOL *protocol=va_arg(args,DALI_USB_RESOURCE_CONTEXT_PROTOCOL *);
  assert(va_arg(args,VOID *)==NULL); va_end(args);
  if (no_publish) { return EFI_OUT_OF_RESOURCES; }
  *handle=(EFI_HANDLE)(UINTN)2; published=protocol; return EFI_SUCCESS;
}
static void reset(void) {
  // Test process cleanup only; firmware deliberately never frees this context.
  free(mContext.Snapshot); memset(&mContext,0,sizeof mContext);
  power_reset(); memset(&mI2c,0,sizeof mI2c);
  memset(desc,0,sizeof desc); memset(cpu_mapped,0,sizeof cpu_mapped);
  memset(cpu_cached,0,sizeof cpu_cached); gcd_sync_shortcut=FALSE;
  for (UINTN i=0;i<10;i++) { desc[i].BaseAddress=pages[i]; desc[i].Length=4096; }
  operation=fail_at=arm_calls=freed=event_calls=install_calls=cache_flushes=0;
  applied_error=no_pool=no_event=no_publish=mutate_copy=bad_address=bad_final=FALSE;
  current_tpl=TPL_APPLICATION; ebs=NULL; ebs_context=NULL; published=NULL;
  memset(&bs,0,sizeof bs); memset(&ds,0,sizeof ds); gBS=&bs; gDS=&ds;
  bs.RaiseTPL=raise_tpl; bs.RestoreTPL=restore_tpl; bs.CreateEventEx=create_event;
  bs.InstallMultipleProtocolInterfaces=publish;
  bs.LocateProtocol=power_locate_bus;
  ds.GetMemorySpaceDescriptor=get_desc; ds.AddMemorySpace=add_space;
  ds.AllocateMemorySpace=allocate_space; ds.SetMemorySpaceAttributes=set_attrs;
  // FreeMemorySpace/RemoveMemorySpace intentionally absent: calling them faults.
  vusb_fixture(); add_i2c_fixture();
}
static void unchanged(EFI_STATUS expected) {
  DALI_USB_CLOCK_RESOURCES out,old; memset(&out,0xa5,sizeof out); old=out;
  assert(published && published->GetClockResources(published,&out)==expected && !memcmp(&out,&old,sizeof out));
  DALI_USB_XHCI_RESOURCES xhci,xhci_old; memset(&xhci,0xa5,sizeof xhci); xhci_old=xhci;
  assert(published->GetXhciResources(published,&xhci)==expected && !memcmp(&xhci,&xhci_old,sizeof xhci));
}
static void entry(EFI_STATUS expected) {
  assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_SUCCESS);
  assert(published && published->Revision==5 && mContext.Status.InitializationStatus==expected && freed==0);
  DALI_USB_RESOURCE_CONTEXT_STATUS status;
  assert(published->GetStatus(published,&status)==EFI_SUCCESS && status.InitializationStatus==expected);
  assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_ALREADY_STARTED && event_calls==1 && install_calls==1);
  if (EFI_ERROR(expected)) { unchanged(expected); }
}
static void success(void) {
  reset(); entry(EFI_SUCCESS); UINTN count=operation;
  assert(count==70 && arm_calls==10 && mContext.PageCount==10 && mContext.Status.MappedPages==10 && mContext.Status.MappingAttempts==10);
  for (UINTN i=0;i<10;i++) { assert(cpu_mapped[i] && desc[i].ImageHandle==image && desc[i].Attributes==EFI_MEMORY_UC); }
  DALI_USB_CLOCK_RESOURCES out; assert(published->GetClockResources(published,&out)==EFI_SUCCESS); check(&out);
  DALI_USB_XHCI_RESOURCES xhci; assert(published->GetXhciResources(published,&xhci)==EFI_SUCCESS);
  assert(xhci.Registers.Base==0x16700000 && xhci.Registers.Size==0x1000);
  assert(cpu_mapped[index_of(xhci.Registers.Base)]);
  assert(mContext.Snapshot!=tree && !memcmp(mContext.Snapshot,tree,sizeof tree));
  memset(tree,0,sizeof tree); assert(published->GetClockResources(published,&out)==EFI_SUCCESS); check(&out);
  assert(DaliUsbReadClockResources(mContext.Snapshot,mContext.SnapshotSize,&out)==EFI_SUCCESS); check(&out);
  assert(published->GetStatus(NULL,NULL)==EFI_INVALID_PARAMETER && published->GetClockResources(NULL,&out)==EFI_INVALID_PARAMETER);
  ebs((EFI_EVENT)(UINTN)1,ebs_context); ebs((EFI_EVENT)(UINTN)1,ebs_context);
  unchanged(EFI_UNSUPPORTED);
  DALI_USB_RESOURCE_CONTEXT_STATUS status,old; memset(&status,0xa5,sizeof status); old=status;
  assert(published->GetStatus(published,&status)==EFI_UNSUPPORTED && !memcmp(&status,&old,sizeof status));
  for (UINTN n=1;n<=count;n++) { for (unsigned applied=0;applied<2;applied++) {
    reset(); fail_at=n; applied_error=(BOOLEAN)applied; entry(EFI_DEVICE_ERROR);
    assert(operation==n && mContext.Status.MappingAttempts>=1 && mContext.Status.MappingAttempts<=10 &&
      mContext.Status.MappedPages<mContext.Status.MappingAttempts && freed==0);
    // A failed mapping is never counted complete, even when it took effect.
    for (UINTN i=0;i<mContext.Status.MappedPages;i++) { assert(cpu_mapped[i] && desc[i].ImageHandle==image); }
  } }
}
static void refusals(void) {
  for (UINTN i=5;i<10;i++) {
    reset(); desc[i].ImageHandle=(EFI_HANDLE)(UINTN)3; entry(EFI_ACCESS_DENIED);
    assert(mContext.Status.MappingAttempts==i+1 && mContext.Status.MappedPages==i && arm_calls==i && pin_reads==0);
  }
  for (unsigned kind=0;kind<8;kind++) {
    reset(); desc[0].GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo; desc[0].Capabilities=EFI_MEMORY_UC;
    switch(kind) {
      case 0: desc[0].ImageHandle=(EFI_HANDLE)(UINTN)3; break;
      case 1: desc[0].DeviceHandle=(EFI_HANDLE)(UINTN)3; break;
      case 2: desc[0].GcdMemoryType=EfiGcdMemoryTypeSystemMemory; break;
      case 3: desc[0].Capabilities=EFI_MEMORY_WB; break;
      case 4: desc[0].Attributes=EFI_MEMORY_WB; break;
      case 5: desc[0].Attributes=EFI_MEMORY_UC|EFI_MEMORY_RO; break;
      case 6: desc[0].Length=4095; break;
      default: desc[0].BaseAddress++; break;
    }
    entry(EFI_ACCESS_DENIED); assert(operation==1 && arm_calls==0);
  }
  reset(); desc[0].Attributes=EFI_MEMORY_UC; entry(EFI_UNSUPPORTED); assert(arm_calls==0);
  reset(); bad_address=TRUE; entry(EFI_COMPROMISED_DATA); assert(arm_calls==0 && desc[0].ImageHandle==image);
  reset(); bad_final=TRUE; entry(EFI_DEVICE_ERROR); assert(arm_calls==1 && cpu_mapped[0]);
  reset(); no_pool=TRUE; entry(EFI_OUT_OF_RESOURCES); assert(operation==0 && !mContext.Status.SnapshotValid);
  reset(); mutate_copy=TRUE; entry(EFI_COMPROMISED_DATA); assert(operation==0 && !mContext.Status.SnapshotValid);
  reset(); text(paths[0],"status","disabled"); entry(EFI_UNSUPPORTED); assert(operation==0);
  reset(); cell(RAIL,"regulator-always-on",1); entry(EFI_UNSUPPORTED); assert(operation==0 && power_commands==0);
  reset(); cell(I2C,"sda-gpio-id",104); entry(EFI_UNSUPPORTED); assert(operation==0 && power_commands==0 && pin_reads==0);
  pin_failure(EFI_UNSUPPORTED); assert(pin_guard_calls==0 && pin_reads==0);
  reset(); fdt_set_magic(tree,0); entry(EFI_COMPROMISED_DATA); assert(operation==0);
  reset(); fdt_set_totalsize(tree,sizeof tree+1); entry(EFI_COMPROMISED_DATA); assert(operation==0);
  reset(); assert(Prepare(tree,39)==EFI_COMPROMISED_DATA && operation==0);
  reset(); no_event=TRUE; assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_SUCCESS &&
    published==NULL && install_calls==0 && mContext.Status.InitializationStatus==EFI_OUT_OF_RESOURCES && arm_calls==10 && freed==0);
  no_event=FALSE; assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_ALREADY_STARTED && install_calls==0);
  reset(); no_publish=TRUE; assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_SUCCESS &&
    published==NULL && ebs && arm_calls==10 && freed==0 && mContext.Status.InitializationStatus==EFI_OUT_OF_RESOURCES);
  no_publish=FALSE; assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_ALREADY_STARTED && install_calls==1);
  ebs(NULL,NULL); assert(mContext.Status.BootServicesClosed);
  reset(); current_tpl=TPL_CALLBACK; assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_SUCCESS &&
    current_tpl==TPL_CALLBACK && published==NULL && event_calls==0 && operation==0);
  current_tpl=TPL_APPLICATION; assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_ALREADY_STARTED && event_calls==0);
  reset(); assert(DaliUsbResourceContextInitialize(NULL,NULL)==EFI_SUCCESS && published==NULL && event_calls==0 && operation==0);
  assert(DaliUsbResourceContextInitialize(image,NULL)==EFI_ALREADY_STARTED && event_calls==0);
}
static void inherited(void) {
  reset();
    for (UINTN i=0;i<10;i++) {
    desc[i].GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo; desc[i].Capabilities=EFI_MEMORY_UC|EFI_MEMORY_WB;
    desc[i].Attributes=EFI_MEMORY_UC|(i%2 ? EFI_MEMORY_XP:0);
  }
  entry(EFI_SUCCESS); assert(operation==50 && arm_calls==10);
  for (UINTN i=0;i<10;i++) { assert(cpu_mapped[i] && desc[i].Attributes==(EFI_MEMORY_UC|(i%2 ? EFI_MEMORY_XP:0))); }
  reset(); // GCD has no cache attributes, but the old CPU mapping is WB.
  for (UINTN i=0;i<10;i++) { cpu_mapped[i]=TRUE; cpu_cached[i]=TRUE; }
  entry(EFI_SUCCESS); assert(cache_flushes==10 && arm_calls==10);
  reset(); // CPU map must still be established if GCD only changes metadata.
  gcd_sync_shortcut=TRUE; entry(EFI_SUCCESS); assert(arm_calls==10);
  for (UINTN i=0;i<10;i++) { assert(cpu_mapped[i]); }
  reset(); desc[5].GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo; desc[5].Capabilities=EFI_MEMORY_UC; desc[5].Attributes=EFI_MEMORY_UC;
  entry(EFI_SUCCESS); assert(arm_calls==10 && operation==68); // Initial platform GPIO page.
}
int main(int argc,char **argv) {
  assert(ClockGraphMain(argc,argv)==0); success(); refusals(); inherited(); power_protocol_tests(); pin_protocol_tests(); native_bus_tests(); reset();
  if (argc==2) {
    FILE *f=fopen(argv[1],"rb"); assert(f && fseek(f,0,SEEK_END)==0); long length=ftell(f); assert(length>40);
    rewind(f); void *saved=malloc((size_t)length); assert(saved && fread(saved,1,(size_t)length,f)==(size_t)length); fclose(f);
    mContext.Image=image; assert(Prepare(saved,(UINTN)length)==EFI_SUCCESS && arm_calls==10); free(saved);
    puts("PASS: saved Dali FDT private context with modeled CPU/GCD mappings");
  }
  free(mContext.Snapshot); mContext.Snapshot=NULL;
  puts("PASS: production USB resource context, real EDK2 ABI/libfdt, modeled GCD/CPU mapping faults and EBS closure; hardware_qualified=false");
  return 0;
}
