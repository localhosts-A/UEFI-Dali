// Real native adapter + unchanged Mu UsbHcMem, with modeled MMIO/DMA/cache/holds.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/DaliUsbXhciIoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include "Xhci.h"
// Actual non-static source functions absent from the driver's public headers.
VOID XhcSetHsee(USB_XHCI_INSTANCE *);
EFI_STATUS XhcFlushAsyncIntMap(USB_XHCI_INSTANCE *,URB *);

static DALI_USB_XHCI_IO c;
static struct {
  UINT32 regs[0x1000/4];
  UINTN ops,ebs_at,allocs,frees,syncs,reads,writes,guards,delays,fail_allocate,fail_sync,fail_free;
  BOOLEAN applied,hold,release,halt_timeout,bad_address,noncoherent,alias,partial;
  UINT64 next_address;
  struct {VOID *cpu;UINT64 address;UINTN size;BOOLEAN live,partial,coherent; } dma[64];
  struct {DALI_USB_XHCI_DMA_DIRECTION direction;BOOLEAN device;UINTN offset,bytes;} sync_trace[128];
  VOID *pool[4096];
} m;
static BOOLEAN before(void) {
  assert(!c.Closed);m.ops++;
  if(m.ebs_at==m.ops && !m.applied) {c.Closed=TRUE;return FALSE;}
  return TRUE;
}
static void after(void) {if(m.ebs_at==m.ops && m.applied)c.Closed=TRUE;}
VOID *EFIAPI AllocatePool(UINTN size) {
  void *p=malloc((size_t)size);assert(p);
  for(unsigned n=0;n<4096;n++) if(!m.pool[n]) {m.pool[n]=p;return p;}
  abort();
}
VOID *EFIAPI AllocateZeroPool(UINTN size) {void *p=AllocatePool(size);memset(p,0,(size_t)size);return p;}
VOID EFIAPI FreePool(VOID *p) {
  for(unsigned n=0;n<4096;n++) if(m.pool[n]==p) {m.pool[n]=NULL;free(p);return;}
  abort();
}
static EFI_STATUS EFIAPI service_free(VOID *p) {FreePool(p);return EFI_SUCCESS;}
static EFI_BOOT_SERVICES services={.FreePool=service_free};
EFI_BOOT_SERVICES *gBS=&services;
VOID *EFIAPI CopyMem(VOID *to,CONST VOID *from,UINTN size) {return memmove(to,from,(size_t)size);}
VOID *EFIAPI ZeroMem(VOID *to,UINTN size) {return memset(to,0,(size_t)size);}
VOID EFIAPI MemoryFence(VOID) {__sync_synchronize();}
UINT8 EFIAPI MmioRead8(UINTN address) {
  assert(address==0x16700000);if(!before())return 0;m.reads++;
  UINT8 value=(UINT8)m.regs[0];after();return value;
}
UINT32 EFIAPI MmioRead32(UINTN address) {
  assert(address>=0x16700000 && address<=0x16700ffc && !(address&3));if(!before())return 0;
  m.reads++;UINT32 value=m.regs[(address-0x16700000)/4];after();return value;
}
UINT8 EFIAPI MmioWrite8(UINTN address,UINT8 value) {
  (void)address;(void)value;abort();
}
UINT32 EFIAPI MmioWrite32(UINTN address,UINT32 value) {
  assert(address>=0x16700000 && address<=0x16700ffc && !(address&3));if(!before())return value;
  m.writes++;m.regs[(address-0x16700000)/4]=value;
  if(address==0x16700040) {
    if(value&1)m.regs[0x44/4]&=~1U;
    else if(!m.halt_timeout)m.regs[0x44/4]|=1;
  }
  after();return value;
}
UINTN EFIAPI MicroSecondDelay(UINTN us) {if(!before())return us;m.delays+=us;after();return us;}
static EFI_STATUS EFIAPI guard(VOID *context,DALI_USB_XHCI_IO_PHASE phase) {
  assert(context==&m);if(!before())return EFI_UNSUPPORTED;m.guards++;
  EFI_STATUS status=(phase==DaliXhciRelease?m.release:m.hold)?EFI_SUCCESS:EFI_NOT_READY;
  after();return status;
}
static EFI_STATUS EFIAPI allocate(VOID *context,UINTN size,UINTN alignment,UINT64 limit,DALI_USB_XHCI_DMA_BUFFER *out) {
  assert(context==&m && limit==0xffffffff && (alignment==4096 || alignment==65536));
  if(!before())return EFI_UNSUPPORTED;
  unsigned n=0;while(n<64 && m.dma[n].live)n++;assert(n<64);
  void *p=NULL;assert(posix_memalign(&p,(size_t)alignment,(size_t)size)==0);memset(p,0xa5,(size_t)size);
  UINT64 address=m.bad_address?0x100000000ULL:m.next_address;
  if(m.alias && m.allocs)address=m.dma[0].address;
  m.next_address+=0x100000;
  m.dma[n].cpu=p;m.dma[n].address=address;m.dma[n].size=size;m.dma[n].live=TRUE;
  m.dma[n].partial=m.partial;m.dma[n].coherent=!m.noncoherent;
  *out=(DALI_USB_XHCI_DMA_BUFFER){p,address,size,&m.dma[n],!m.noncoherent};m.allocs++;
  if(m.partial)*out=(DALI_USB_XHCI_DMA_BUFFER){.Cookie=&m.dma[n]};
  after();return m.fail_allocate==m.allocs?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static unsigned buffer_index(DALI_USB_XHCI_DMA_BUFFER *b) {
  for(unsigned n=0;n<64;n++) if(b->Cookie==&m.dma[n]) {
    if(m.dma[n].partial) {assert(m.dma[n].live && !b->Cpu && !b->Address && !b->Size && !b->Coherent);return n;}
    assert(m.dma[n].live && b->Cpu==m.dma[n].cpu && b->Address==m.dma[n].address && b->Size==m.dma[n].size);return n;
  }
  abort();
}
static EFI_STATUS EFIAPI sync_buffer(VOID *context,DALI_USB_XHCI_DMA_BUFFER *b,UINTN offset,UINTN bytes,
  DALI_USB_XHCI_DMA_DIRECTION direction,BOOLEAN device) {
  assert(context==&m);(void)buffer_index(b);assert(bytes && offset<=b->Size && bytes<=b->Size-offset);
  assert(direction<=DaliXhciBidirectional && b->Coherent==m.dma[buffer_index(b)].coherent);if(!before())return EFI_UNSUPPORTED;
  assert(m.syncs<128);m.sync_trace[m.syncs].direction=direction;m.sync_trace[m.syncs].device=device;
  m.sync_trace[m.syncs].offset=offset;m.sync_trace[m.syncs].bytes=bytes;
  m.syncs++;after();return m.fail_sync==m.syncs?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_buffer(VOID *context,DALI_USB_XHCI_DMA_BUFFER *b) {
  assert(context==&m);unsigned n=buffer_index(b);if(!before())return EFI_UNSUPPORTED;
  m.frees++;
  if(m.fail_free==m.frees) {after();return EFI_DEVICE_ERROR;}
  m.dma[n].live=FALSE;free(m.dma[n].cpu);after();return EFI_SUCCESS;
}
static void reset(void) {
  // Host-only fresh-boot disposal, never a production cleanup after EBS.
  for(unsigned n=0;n<64;n++) if(m.dma[n].live)free(m.dma[n].cpu);
  for(unsigned n=0;n<4096;n++)free(m.pool[n]);
  memset(&m,0,sizeof m);memset(&c,0,sizeof c);m.hold=m.release=TRUE;m.next_address=0x200000;
  m.regs[0]=0x40;m.regs[0x44/4]=1;
}
static EFI_STATUS open_io(void) {
  DALI_USB_XHCI_DMA_OPS dma={&m,TRUE,allocate,sync_buffer,free_buffer};
  return DaliUsbXhciIoOpen(&c,0x16700000,0x1000,&dma,guard,&m);
}
static void close_io(void) {
  assert(DaliUsbXhciIoClose(&c)==EFI_SUCCESS && !c.Initialized && !c.RecordCount && !c.AllocatedBytes);
  for(unsigned n=0;n<64;n++)assert(!m.dma[n].live);
}
static EFI_STATUS alloc_pages(UINTN pages,VOID **out) {
  return c.Io.AllocateBuffer(&c.Io,AllocateAnyPages,EfiBootServicesData,pages,out,0);
}
static void common_and_source(void) {
  reset();assert(open_io()==EFI_SUCCESS && !m.writes);
  USBHC_MEM_POOL *pool=UsbHcInitMemPool(&c.Io);assert(pool && m.allocs==1);
  assert(m.sync_trace[0].direction==DaliXhciBidirectional && m.sync_trace[0].device);
  VOID *ring=UsbHcAllocateMem(pool,4096,TRUE);assert(ring);
  EFI_PHYSICAL_ADDRESS address=UsbHcGetPciAddrForHostAddr(pool,ring,4096,TRUE);
  assert(address<=0xffffffff && (UINT64)(UINTN)ring!=address && (address&0xffff)==((UINTN)ring&0xffff));
  assert(UsbHcGetHostAddrForPciAddr(pool,(VOID *)(UINTN)address,4096,TRUE)==(UINT64)(UINTN)ring);
  UsbHcFreeMem(pool,ring,4096);UsbHcFreeMemPool(pool);
  assert(m.sync_trace[1].direction==DaliXhciBidirectional && !m.sync_trace[1].device);
  assert(m.allocs==m.frees && !c.Records);
  VOID *host=NULL,*mapping=NULL;
  assert(UsbHcAllocateAlignedPages(&c.Io,16,65536,&host,&address,&mapping)==EFI_SUCCESS && !((UINTN)host&65535) && !(address&65535));
  UsbHcFreeAlignedPages(&c.Io,host,16,mapping);assert(!c.Records);
  assert(alloc_pages(3,&host)==EFI_SUCCESS);VOID *middle=(UINT8 *)host+4096;
  assert(c.Io.FreeBuffer(&c.Io,1,host)==EFI_SUCCESS && c.Io.FreeBuffer(&c.Io,1,(UINT8 *)host+8192)==EFI_SUCCESS);
  UINTN bytes=4096;assert(c.Io.Map(&c.Io,EfiPciIoOperationBusMasterCommonBuffer,middle,&bytes,&address,&mapping)==EFI_SUCCESS);
  assert((address&65535)==4096 && c.Io.Unmap(&c.Io,mapping)==EFI_SUCCESS && c.Io.FreeBuffer(&c.Io,1,middle)==EFI_SUCCESS);
  close_io();
}
static void streaming(void) {
  reset();assert(open_io()==EFI_SUCCESS);UINT8 host[67];for(unsigned n=0;n<67;n++)host[n]=(UINT8)n;
  UINTN bytes=65;EFI_PHYSICAL_ADDRESS address=0;VOID *mapping=NULL;
  assert(c.Io.Map(&c.Io,EfiPciIoOperationBusMasterRead,host+1,&bytes,&address,&mapping)==EFI_SUCCESS);
  assert(!memcmp(m.dma[0].cpu,host+1,65) && !((UINT8 *)m.dma[0].cpu)[65]);
  memset(host,0xff,sizeof host);assert(c.Io.Unmap(&c.Io,mapping)==EFI_SUCCESS && host[1]==0xff);
  bytes=65;assert(c.Io.Map(&c.Io,EfiPciIoOperationBusMasterWrite,host+1,&bytes,&address,&mapping)==EFI_SUCCESS);
  for(unsigned n=0;n<4096;n++)assert(((UINT8 *)m.dma[0].cpu)[n]==0);
  memset(m.dma[0].cpu,0x7b,65);assert(c.Io.Unmap(&c.Io,mapping)==EFI_SUCCESS && host[0]==0xff && host[66]==0xff);
  for(unsigned n=1;n<66;n++)assert(host[n]==0x7b);
  assert(m.syncs==4 && m.allocs==m.frees && c.Io.Flush(&c.Io)==EFI_SUCCESS);close_io();
  for(unsigned n=0;n<4;n++)assert(m.sync_trace[n].direction==(n<2?DaliXhciToDevice:DaliXhciFromDevice) &&
    m.sync_trace[n].device==(BOOLEAN)(n%2==0) && m.sync_trace[n].offset==0 && m.sync_trace[n].bytes==65);
}
static void retained_faults(void) {
  reset();assert(open_io()==EFI_SUCCESS);USBHC_MEM_POOL *pool=UsbHcInitMemPool(&c.Io);assert(pool);
  m.fail_sync=m.syncs+1;UsbHcFreeMemPool(pool);assert(c.Faulted && c.Records && !m.frees);
  m.fail_sync=0;close_io();assert(m.frees==1);
  reset();assert(open_io()==EFI_SUCCESS);pool=UsbHcInitMemPool(&c.Io);assert(pool);
  m.fail_free=1;UsbHcFreeMemPool(pool);assert(c.Faulted && c.Records && m.dma[0].live);
  m.fail_free=0;close_io();assert(m.frees==2);
  for(unsigned what=0;what<5;what++) {
    reset();assert(open_io()==EFI_SUCCESS);VOID *host=(VOID *)(UINTN)0xa5;
    if(what==0)m.bad_address=TRUE;else if(what==1)m.noncoherent=TRUE;else if(what==2)m.fail_allocate=1;
    else if(what==3)m.fail_sync=1;else {m.partial=TRUE;m.fail_allocate=1;}
    EFI_STATUS status=alloc_pages(1,&host);
    if(what==3) {assert(status==EFI_SUCCESS);UINTN bytes=4096;EFI_PHYSICAL_ADDRESS address=0xa5;VOID *map=(VOID *)(UINTN)0xa5;
      assert(c.Io.Map(&c.Io,EfiPciIoOperationBusMasterCommonBuffer,host,&bytes,&address,&map)==EFI_DEVICE_ERROR && address==0xa5 && map==(VOID *)(UINTN)0xa5);
    } else assert(EFI_ERROR(status) && host==(VOID *)(UINTN)0xa5);
    assert(c.Faulted && c.Records);m.fail_allocate=m.fail_sync=0;close_io();
  }
  reset();assert(open_io()==EFI_SUCCESS);VOID *host=NULL;assert(alloc_pages(1,&host)==EFI_SUCCESS);
  m.alias=TRUE;VOID *second=(VOID *)(UINTN)0xa5;assert(alloc_pages(1,&second)==EFI_ACCESS_DENIED && second==(VOID *)(UINTN)0xa5);close_io();
  reset();assert(open_io()==EFI_SUCCESS);assert(alloc_pages(1,&host)==EFI_SUCCESS);
  UINTN bytes=8;EFI_PHYSICAL_ADDRESS address;VOID *mapping;
  assert(c.Io.Map(&c.Io,EfiPciIoOperationBusMasterWrite,host,&bytes,&address,&mapping)==EFI_SUCCESS);
  assert(c.Io.FreeBuffer(&c.Io,1,host)==EFI_ACCESS_DENIED && !m.frees && c.Faulted);close_io();
}
static void actual_schedule(void) {
  reset();assert(open_io()==EFI_SUCCESS);USB_XHCI_INSTANCE xhc={0};xhc.PciIo=&c.Io;xhc.CapLength=0x40;
  assert(XhcReadCapReg8(&xhc,0)==0x40 && XhcReadOpReg(&xhc,4)==1);
  UINTN writes=m.writes;XhcSetHsee(&xhc);assert(m.writes==writes && !c.Faulted);
  UINT8 host[8]={0};URB urb={0};urb.Data=host;urb.DataLen=8;urb.Ep.Direction=EfiUsbDataIn;
  assert(XhcFlushAsyncIntMap(&xhc,&urb)==EFI_SUCCESS && urb.DataMap && !host[0]);
  memset(m.dma[0].cpu,0x5a,8);VOID *old=urb.DataPhy;
  assert(XhcFlushAsyncIntMap(&xhc,&urb)==EFI_SUCCESS && urb.DataMap && urb.DataPhy!=old);
  for(unsigned n=0;n<8;n++)assert(host[n]==0x5a);
  assert(m.syncs==3 && m.sync_trace[0].direction==DaliXhciFromDevice && m.sync_trace[0].device &&
    m.sync_trace[1].direction==DaliXhciFromDevice && !m.sync_trace[1].device && m.sync_trace[2].device);
  m.fail_sync=m.syncs+1;VOID *retained=urb.DataMap;
  assert(XhcFlushAsyncIntMap(&xhc,&urb)==EFI_DEVICE_ERROR && urb.DataMap==retained && c.Faulted && m.dma[0].live);
  m.fail_sync=0;close_io();
}
static void stop_and_bounds(void) {
  reset();m.regs[0x40/4]=1;m.regs[0x44/4]=0;assert(open_io()==EFI_NOT_READY && !c.Owned && !m.writes);
  assert(DaliUsbXhciIoQuiesce(&c,10)==EFI_UNSUPPORTED);close_io();assert(!m.writes);
  reset();assert(open_io()==EFI_SUCCESS);VOID *host=NULL;assert(alloc_pages(1,&host)==EFI_SUCCESS);
  UINT32 command=0x0d;assert(c.Io.Mem.Write(&c.Io,EfiPciIoWidthUint32,0,0x40,1,&command)==EFI_SUCCESS);
  m.halt_timeout=TRUE;m.release=FALSE;assert(DaliUsbXhciIoClose(&c)==EFI_NOT_READY && !m.frees);
  m.release=TRUE;assert(DaliUsbXhciIoQuiesce(&c,25)==EFI_TIMEOUT && m.delays==25 && c.Records && !c.HaltProven);
  assert(c.Io.FreeBuffer(&c.Io,1,host)==EFI_DEVICE_ERROR && !m.frees);
  m.halt_timeout=FALSE;close_io();
  for(unsigned what=0;what<4;what++) {
    reset();assert(open_io()==EFI_SUCCESS);UINT32 value=0xa5a5;UINTN reads=m.reads;
    assert(c.Io.Mem.Read(&c.Io,what==0?EfiPciIoWidthUint64:EfiPciIoWidthUint32,what==1?1:0,
      what==2?0xffe:0x40,what==3?2:1,&value)==EFI_INVALID_PARAMETER && value==0xa5a5 && m.reads==reads);close_io();
  }
}
static void setup(unsigned phase) {
  reset();if(phase==0)return;assert(open_io()==EFI_SUCCESS);
  if(phase>=2) {UINT8 host[8]={0};UINTN bytes=sizeof host;EFI_PHYSICAL_ADDRESS address;VOID *mapping;
    // Failed cleanup must not later dereference this deliberately expired host.
    assert(c.Io.Map(&c.Io,EfiPciIoOperationBusMasterWrite,host,&bytes,&address,&mapping)==EFI_SUCCESS);
  }
}
static EFI_STATUS run(unsigned phase) {
  if(phase==0)return open_io();
  if(phase==1) {UINT8 host[8]={0};UINTN bytes=8;EFI_PHYSICAL_ADDRESS address=0xa5;VOID *map=(VOID *)(UINTN)0xa5;
    EFI_STATUS status=c.Io.Map(&c.Io,EfiPciIoOperationBusMasterWrite,host,&bytes,&address,&map);
    if(EFI_ERROR(status)) {assert(address==0xa5 && map==(VOID *)(UINTN)0xa5);}
    return status;
  }
  if(phase==2)return DaliUsbXhciIoQuiesce(&c,10);
  return DaliUsbXhciIoClose(&c);
}
static void ebs(void) {
  for(unsigned phase=0;phase<4;phase++) {
    setup(phase);UINTN base=m.ops;assert(run(phase)==EFI_SUCCESS);UINTN total=m.ops-base;
    for(UINTN at=1;at<=total;at++)for(unsigned applied=0;applied<2;applied++) {
      setup(phase);m.ebs_at=m.ops+at;m.applied=(BOOLEAN)applied;
      assert(run(phase)==EFI_UNSUPPORTED && c.Closed && c.Initialized && m.ops==m.ebs_at);
      UINTN ops=m.ops;assert(DaliUsbXhciIoClose(&c)==EFI_UNSUPPORTED && m.ops==ops);
    }
    printf("PASS: native xHCI IO phase%u EBS before/applied at %llu provider/MMIO/hold positions\n",phase,(unsigned long long)total);
  }
}
int main(void) {
  common_and_source();streaming();retained_faults();actual_schedule();stop_and_bounds();ebs();reset();
  puts("PASS: production native xHCI MMIO/32-bit private DMA ABI with actual Mu UsbHcMem, translated coherent pools, bounce/cache, retained faults/halt/EBS models; no board provider/HC binding/hardware qualification");
  return 0;
}
