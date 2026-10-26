#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#include "UfsLogConsole.h"
EFI_BOOT_SERVICES *gBS;
static EFI_TPL tpl;
static unsigned mode,allocs,frees,reads,writes,syncs,snapshots;
static BOOLEAN revoked;
static UINT8 disk[1048576],expected[1048576];
static MT6991_UFS_SCRATCH_GRANT grant;
static EFI_BLOCK_IO_PROTOCOL io;
static BOOLEAN (*backend)(CONST VOID *,UINTN,UINT8 *);
VOID set_console_backend(BOOLEAN (*b)(CONST VOID *,UINTN,UINT8 *)){backend=b;}
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n){return memcpy(d,s,n);}
VOID *EFIAPI ZeroMem(VOID *d,UINTN n){return memset(d,0,n);}
INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n){return memcmp(a,b,n);}
VOID EFIAPI MemoryFence(VOID){}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return tpl;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL n){EFI_TPL old=tpl;if(n<tpl)abort();tpl=n;if(n==TPL_HIGH_LEVEL)++snapshots;return old;}
static VOID EFIAPI restore_tpl(EFI_TPL n){if(n>tpl)abort();tpl=n;}
VOID *EFIAPI AllocateAlignedPages(UINTN n,UINTN align){
 if(tpl!=TPL_APPLICATION)abort();if(mode==2)return NULL;VOID *p=NULL;
 if(posix_memalign(&p,align,n*4096))return NULL;++allocs;if(mode==7)revoked=TRUE;return p;
}
VOID EFIAPI FreeAlignedPages(VOID *p,UINTN n){(void)n;if(tpl!=TPL_APPLICATION)abort();++frees;free(p);}
VOID *EFIAPI AllocatePool(UINTN n){if(tpl!=TPL_APPLICATION)abort();if(mode==3)return NULL;VOID *p=malloc(n);if(p)++allocs;return p;}
VOID EFIAPI FreePool(VOID *p){if(tpl!=TPL_APPLICATION)abort();++frees;free(p);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *p,UINTN n,UINT8 *out){if(tpl!=TPL_APPLICATION)abort();return backend(p,n,out);}
EFI_STATUS Mt6991UfsDmaResolve(MT6991_UFS_DMA *d,UINT64 a,UINTN n,BOOLEAN c,VOID **p){
 (void)d;(void)a;(void)n;(void)c;(void)p;return EFI_NOT_FOUND;
}
EFI_STATUS DaliUfsScratchOwnedIo(CONST MT6991_UFS_SCRATCH_GRANT *g,EFI_BLOCK_IO_PROTOCOL **out){
 *out=NULL;if(g!=&grant||mode==1)return EFI_ACCESS_DENIED;*out=&io;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO_PROTOCOL *self,UINT32 id,EFI_LBA lba,UINTN n,VOID *p){
 if(tpl!=TPL_APPLICATION||self!=&io||id!=3||lba*4096+n>sizeof disk)abort();++reads;
 if(revoked)return EFI_NO_MEDIA;memcpy(p,disk+lba*4096,n);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_blocks(EFI_BLOCK_IO_PROTOCOL *self,UINT32 id,EFI_LBA lba,UINTN n,VOID *p){
 if(tpl!=TPL_APPLICATION||self!=&io||id!=3||lba*4096+n>sizeof disk)abort();++writes;
 if(revoked)return EFI_NO_MEDIA;memcpy(disk+lba*4096,p,n);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_BLOCK_IO_PROTOCOL *self){if(tpl!=TPL_APPLICATION||self!=&io)abort();++syncs;return EFI_SUCCESS;}
VOID run_console(unsigned fault,UINT64 *out,UINT8 *digest){
 mode=fault;allocs=frees=reads=writes=syncs=snapshots=0;revoked=FALSE;tpl=TPL_APPLICATION;
 static EFI_BOOT_SERVICES bs={.RaiseTPL=raise_tpl,.RestoreTPL=restore_tpl};gBS=&bs;
 memset(disk,0,sizeof disk);memset(&grant,0,sizeof grant);
 grant.Enabled=TRUE;grant.Lun=2;memset(grant.Token,'a',32);memset(grant.DeviceBinding,1,32);
 memset(grant.CandidateSha256,2,32);memset(grant.SourceSha256,3,32);memset(grant.PartitionGuid,4,16);
 grant.BlockSize=4096;grant.CapacityBlocks=64577536;grant.BootStartLba=2184;grant.BootBlocks=32768;
 grant.ScratchStartLba=2440;grant.ScratchBlocks=256;grant.TailGuardBlocks=32256;grant.Target=1;
 EFI_BLOCK_IO_MEDIA media={.MediaId=3,.MediaPresent=TRUE,.LogicalPartition=TRUE,.BlockSize=4096,.LastBlock=255,.IoAlign=4096};
 io=(EFI_BLOCK_IO_PROTOCOL){.Media=&media,.ReadBlocks=read_blocks,.WriteBlocks=write_blocks,.FlushBlocks=flush};
 UINTN capacity=fault==5?1048576:1024;
 DALI_RAM_CONSOLE *ring=malloc(12+capacity);if(!ring)abort();ring->Signature=0x43474244;
 ring->Size=fault==4?0:fault==5?(UINT32)capacity:700;ring->Start=ring->Size;
 if(ring->Size==capacity)ring->Start=19;
 for(UINTN i=0;i<capacity;i++)ring->Data[i]=(UINT8)(i%251);
 if(fault==6)ring->Signature=0;
 if(fault==8){ring->Size=1024;ring->Start=317;}
 if(fault==9)ring->Start=1;
 UINTN first=ring->Size==capacity?ring->Start:0;
 for(UINTN i=0;i<ring->Size;i++)expected[i]=ring->Data[(first+i)%capacity];
 DALI_UFS_LOG_CONSOLE state={0};
 out[0]=DaliUfsLogConsoleRun(&state,&grant,1,FALSE,ring,capacity)&0x7fffffffffffffffULL;
 out[1]=reads;out[2]=writes;out[3]=syncs;out[4]=snapshots;out[5]=state.Record.Verified;
 out[6]=state.Record.BodyBytes;out[7]=state.Record.Body==NULL;memcpy(digest,state.Record.BodySha256,32);
 out[8]=DaliUfsLogConsoleRun(&state,&grant,1,FALSE,ring,capacity)&0x7fffffffffffffffULL;
 if(state.Record.Verified&&memcmp(disk+8192,expected,state.Record.BodyBytes))abort();
 // New firmware boot, explicit readback, no ring or payload allocation needed.
 mode=0;revoked=FALSE;reads=writes=syncs=snapshots=0;
 DALI_UFS_LOG_CONSOLE second={0};
 out[9]=DaliUfsLogConsoleRun(&second,&grant,1,TRUE,NULL,0)&0x7fffffffffffffffULL;
 out[10]=reads;out[11]=writes;out[12]=syncs;out[13]=snapshots;out[14]=second.Record.Verified;
 out[15]=allocs;out[16]=frees;out[17]=tpl;
 free(ring);
}
