// Native harness uses real Mu protocol headers and the complete production TU.
#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "UfsScratch.c"

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiBlockIoProtocolGuid = {1}, gEfiScsiIoProtocolGuid = {2}, gEfiDevicePathProtocolGuid = {3};
static EFI_TPL tpl;
static unsigned mode, services, submits, installs, removes, allocated, freed, policy_errors, layout_reads;
static UINT8 opcodes[8], lengths[8];
static UINT64 lbas[8], syncs[8];
static BOOLEAN closed;
static MT6991_UFS_SCRATCH_GRANT grant;
static MT6991_UFS_GEOMETRY geometry[8];
static EFI_BLOCK_IO_PROTOCOL parent;
static EFI_BLOCK_IO_MEDIA media;
static EFI_SCSI_IO_PROTOCOL scsi;
static UINT8 parent_path[24], root_path[30], duplicate_path[30];
static EFI_STATUS reentrant;
static BOOLEAN after_publish;
static UINT8 gpt_primary[4096],gpt_backup[4096],gpt_entries[4096];

static void put32(UINT8 *p,UINT32 v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
static void put64(UINT8 *p,UINT64 v){for(unsigned i=0;i<8;i++)p[i]=v>>(i*8);}
static UINT32 crc(CONST UINT8 *p,unsigned n){
 UINT32 c=MAX_UINT32;for(unsigned i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320U:0);}return ~c;
}
static void layout_fixture(void){
 memset(gpt_entries,0,sizeof gpt_entries);memset(gpt_entries,1,16);memset(gpt_entries+16,4,16);
 put64(gpt_entries+32,grant.BootStartLba);put64(gpt_entries+40,grant.BootStartLba+grant.BootBlocks-1);
 const char *name=grant.Target==MT6991_UFS_TARGET_EXPDB?"expdb":"boot_b";
 for(unsigned i=0;i<strlen(name);i++)gpt_entries[56+2*i]=name[i];
 unsigned block=grant.BlockSize,array_blocks=(512+block-1)/block;
 for(unsigned i=0;i<2;i++){
  UINT8 *h=i?gpt_backup:gpt_primary;memset(h,0,4096);memcpy(h,"EFI PART",8);
  put32(h+8,0x10000);put32(h+12,92);put64(h+24,i?grant.CapacityBlocks-1:1);
  put64(h+32,i?1:grant.CapacityBlocks-1);put64(h+40,2+array_blocks);
  put64(h+48,grant.CapacityBlocks-2-array_blocks);put64(h+72,i?grant.CapacityBlocks-1-array_blocks:2);
  put32(h+80,4);put32(h+84,128);put32(h+88,crc(gpt_entries,512));put32(h+16,crc(h,92));
 }
 if(mode==33)gpt_backup[16]^=1;
}
static EFI_STATUS EFIAPI root_read(EFI_BLOCK_IO_PROTOCOL *self,UINT32 id,EFI_LBA lba,UINTN bytes,VOID *buffer){
 ++layout_reads;unsigned block=grant.BlockSize,array_bytes=((512+block-1)/block)*block;
 if(self!=&parent||id!=media.MediaId||((UINTN)buffer&7))abort();
 if(mode==34&&layout_reads==3)return EFI_TIMEOUT;
 if(mode==35&&layout_reads==2)++media.MediaId;
 if(mode==36&&layout_reads==3)reentrant=DaliUfsScratchUnpublish();
 if(mode==38&&layout_reads==3)grant.Token[0]='f';
 if(lba==1&&bytes==block)memcpy(buffer,gpt_primary,bytes);
 else if(lba==grant.CapacityBlocks-1&&bytes==block)memcpy(buffer,gpt_backup,bytes);
 else if((lba==2||lba==grant.CapacityBlocks-1-array_bytes/block)&&bytes==array_bytes)memcpy(buffer,gpt_entries,bytes);
 else abort();
 return EFI_SUCCESS;
}

VOID *EFIAPI CopyMem (VOID *a, CONST VOID *b, UINTN n) { return memcpy(a,b,n); }
VOID *EFIAPI ZeroMem (VOID *a, UINTN n) { return memset(a,0,n); }
INTN EFIAPI CompareMem (CONST VOID *a, CONST VOID *b, UINTN n) { return memcmp(a,b,n); }
VOID *EFIAPI AllocateAlignedPages (UINTN n, UINTN align) {
  VOID *p = NULL; if (mode == 16) { return NULL; }
  if (posix_memalign(&p,align,n*4096)) { return NULL; } ++allocated; return p;
}
VOID EFIAPI FreeAlignedPages (VOID *p, UINTN n) { (VOID)n; ++freed; free(p); }
VOID EFIAPI FreePool (VOID *p) { ++freed; free(p); }
EFI_TPL EFIAPI EfiGetCurrentTpl (VOID) { return tpl; }
static EFI_TPL EFIAPI raise_tpl (EFI_TPL n) { EFI_TPL old=tpl; if(n<tpl)abort();tpl=n;return old; }
static VOID EFIAPI restore_tpl (EFI_TPL n) { if(n>tpl)abort();tpl=n; }
UINTN EFIAPI GetDevicePathSize (CONST EFI_DEVICE_PATH_PROTOCOL *p) { return (CONST VOID *)p==parent_path?24:30; }
UINT8 EFIAPI DevicePathType (CONST VOID *p) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)p)->Type & 0x7f; }
UINT8 EFIAPI DevicePathSubType (CONST VOID *p) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)p)->SubType; }
UINTN EFIAPI DevicePathNodeLength (CONST VOID *p) {
  CONST EFI_DEVICE_PATH_PROTOCOL *n=p;return n->Length[0]|((UINT16)n->Length[1]<<8);
}
BOOLEAN EFIAPI IsDevicePathEnd (CONST VOID *p) { return DevicePathType(p)==0x7f&&DevicePathSubType(p)==0xff; }
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode (CONST EFI_DEVICE_PATH_PROTOCOL *p, CONST EFI_DEVICE_PATH_PROTOCOL *node) {
  (VOID)p;(VOID)node; VOID *result=malloc(50); if(result)++allocated; return result;
}
static EFI_STATUS EFIAPI handle (EFI_HANDLE h, EFI_GUID *g, VOID **out) {
  ++services;
  if(h==(VOID*)99 && g==&gEfiDevicePathProtocolGuid){*out=parent_path;return EFI_SUCCESS;}
  if(h!=(VOID*)1 && h!=(VOID*)2)return EFI_NOT_FOUND;
  if(mode==28&&after_publish&&g!=&gEfiDevicePathProtocolGuid)return EFI_NOT_FOUND;
  if(g==&gEfiDevicePathProtocolGuid){*out=h==(VOID*)1?root_path:duplicate_path;return EFI_SUCCESS;}
  if(g==&gEfiBlockIoProtocolGuid){*out=&parent;return EFI_SUCCESS;}
  if(g==&gEfiScsiIoProtocolGuid){*out=&scsi;return mode==15?EFI_UNSUPPORTED:EFI_SUCCESS;}
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI locate (EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *g, VOID *key, UINTN *count, EFI_HANDLE **out) {
  (VOID)type;(VOID)g;(VOID)key; ++services;
  *count=mode==14?2:1; *out=malloc(*count*sizeof **out); if(!*out)return EFI_OUT_OF_RESOURCES;
  ++allocated; (*out)[0]=(VOID*)1;if(*count==2)(*out)[1]=(VOID*)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pool (VOID *p) { FreePool(p); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI install (EFI_HANDLE *h, ...) {
  ++installs;if(mode==17)return EFI_OUT_OF_RESOURCES;*h=(VOID*)123;
  if(mode==37)reentrant=DaliUfsScratchUnpublish();return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall (EFI_HANDLE h, ...) {
  (VOID)h;++removes;return mode==18?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
EFI_STATUS Mt6991UfsDmaResolve (MT6991_UFS_DMA *d, UINT64 a, UINTN n, BOOLEAN c, VOID **p) {
  (VOID)d;(VOID)a;(VOID)n;(VOID)c;(VOID)p;return EFI_NOT_FOUND;
}
static UINT64 be (CONST UINT8 *b, unsigned n) { UINT64 v=0;for(unsigned i=0;i<n;i++)v=(v<<8)|b[i];return v; }
static EFI_STATUS EFIAPI execute (EFI_SCSI_IO_PROTOCOL *self, EFI_SCSI_IO_SCSI_REQUEST_PACKET *p, EFI_EVENT event) {
  (VOID)self; UINT8 *c=p->Cdb; unsigned i=submits++;
  if(i>=8)abort();opcodes[i]=c[0];lengths[i]=p->CdbLength;
  if(c[0]==0x28||c[0]==0x2a)lbas[i]=be(c+2,4);
  if(c[0]==0x88||c[0]==0x8a)lbas[i]=be(c+2,8);
  if(event||p->Timeout!=30000000||((UINTN)p->SenseData&7)||p->SenseDataLength!=32)++policy_errors;
  EFI_STATUS s=(c[0]==0x2a||c[0]==0x8a)?
    Mt6991UfsScratchWriteCommand(geometry,&grant,grant.Lun,c,p->CdbLength,p->OutTransferLength):
    Mt6991UfsReadOnlyCommand(geometry,grant.Lun,c,p->CdbLength,p->InTransferLength);
  if(s!=EFI_SUCCESS)++policy_errors;
  if(mode==21)reentrant=mScratch.Io.FlushBlocks(&mScratch.Io);
  // Match pinned Mu: sync success updates target, not host adapter status.
  if(mode!=29)p->TargetStatus=0;
  p->SenseDataLength=0;
  if(mode==30){EFI_TPL old=tpl;tpl=TPL_APPLICATION;reentrant=DaliUfsScratchUnpublish();tpl=old;}
  if(mode==4)return EFI_TIMEOUT;
  if(mode==24&&p->OutTransferLength)return EFI_TIMEOUT;
  if(mode==25&&p->OutTransferLength)--p->OutTransferLength;
  if(mode==26&&c[0]==0x35)return EFI_TIMEOUT;
  if(mode==27&&p->OutTransferLength)return 1; // warning is not successful completion
  if(mode==5){if(p->OutTransferLength)--p->OutTransferLength;else if(p->InTransferLength)--p->InTransferLength;}
  if(mode==6)p->HostAdapterStatus=1;
  if(mode==7)p->TargetStatus=2;
  if(mode==8)++media.MediaId;
  if(c[0]==0x35){
    if(p->CdbLength!=10||p->InTransferLength||p->OutTransferLength||p->InDataBuffer||p->OutDataBuffer)++policy_errors;
    for(unsigned j=1;j<10;j++)if(c[j])++policy_errors;
    if(mode!=9)syncs[grant.Lun]+=mode==31?2:1;
  }
  return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES bs={.RaiseTPL=raise_tpl,.RestoreTPL=restore_tpl,.HandleProtocol=handle,
  .LocateHandleBuffer=locate,.FreePool=pool,.InstallMultipleProtocolInterfaces=install,
  .UninstallMultipleProtocolInterfaces=uninstall};
static UINT64 code (EFI_STATUS s) { return s & 0x7fffffffffffffffULL; }

VOID run (unsigned which, UINT64 *out) {
  mode=which; services=submits=installs=removes=allocated=freed=policy_errors=layout_reads=0;reentrant=0;after_publish=FALSE;
  memset(&mScratch,0,sizeof mScratch);memset(geometry,0,sizeof geometry);memset(&grant,0,sizeof grant);
  memset(syncs,0,sizeof syncs);memset(opcodes,0,sizeof opcodes);memset(lbas,0,sizeof lbas);memset(lengths,0,sizeof lengths);
  tpl=TPL_APPLICATION;closed=FALSE;gBS=&bs;
  grant.Enabled=which!=0;grant.Lun=2;memcpy(grant.Token,"0123456789abcdef0123456789abcdef",33);
  memset(grant.DeviceBinding,1,32);memset(grant.CandidateSha256,2,32);memset(grant.SourceSha256,3,32);memset(grant.PartitionGuid,4,16);
  grant.BlockSize=which==2?512:4096;grant.CapacityBlocks=64577536;
  grant.BootStartLba=418560;grant.BootBlocks=64*1024*1024/grant.BlockSize;grant.CandidateBytes=5062656;
  if(which==3){grant.CapacityBlocks=0x200000000ULL;grant.BootStartLba=0x100000000ULL;}
  grant.ScratchBlocks=grant.TailGuardBlocks=1024*1024/grant.BlockSize;
  grant.ScratchStartLba=grant.BootStartLba+grant.BootBlocks-2*grant.ScratchBlocks;
  if(which==40){
   grant.Target=MT6991_UFS_TARGET_EXPDB;grant.BootStartLba=2184;grant.BootBlocks=32768;grant.CandidateBytes=0;
   grant.ScratchStartLba=2440;grant.TailGuardBlocks=32256;
  }
  geometry[2]=(MT6991_UFS_GEOMETRY){grant.CapacityBlocks,grant.BlockSize,TRUE,TRUE,FALSE};
  media=(EFI_BLOCK_IO_MEDIA){.MediaId=123,.MediaPresent=TRUE,.ReadOnly=TRUE,.BlockSize=grant.BlockSize,.IoAlign=8,.LastBlock=grant.CapacityBlocks-1};
  parent=(EFI_BLOCK_IO_PROTOCOL){.Media=&media,.ReadBlocks=root_read};scsi=(EFI_SCSI_IO_PROTOCOL){.IoAlign=8,.ExecuteScsiCommand=execute};
  layout_fixture();
  memset(parent_path,0,sizeof parent_path);parent_path[0]=1;parent_path[1]=4;parent_path[2]=20;parent_path[4]=0xa5;
  parent_path[20]=0x7f;parent_path[21]=0xff;parent_path[22]=4;
  memcpy(root_path,parent_path,20);root_path[20]=3;root_path[21]=MSG_UFS_DP;root_path[22]=6;root_path[23]=0;root_path[24]=0;root_path[25]=2;
  root_path[26]=0x7f;root_path[27]=0xff;root_path[28]=4;root_path[29]=0;memcpy(duplicate_path,root_path,30);
  if(which==11)geometry[2].WriteProtectionKnown=FALSE;
  if(which==12)media.ReadOnly=FALSE;
  if(which==13)scsi.IoAlign=3;
  if(which==19)memset(grant.CandidateSha256,0,32);
  out[0]=code(DaliUfsScratchPublish((VOID*)99,&grant,geometry,&closed,syncs));
  out[1]=services;out[2]=installs;
  if(out[0]==0){
    after_publish=TRUE;
    VOID *buffer=NULL;if(posix_memalign(&buffer,4096,1024*1024))abort();
    if(which==10)grant.Token[0]='f';
    if(which==20)geometry[2].WriteProtected=TRUE;
    if(which==22)closed=TRUE;
    if(which==23)tpl=TPL_NOTIFY;
    if(which==32)syncs[grant.Lun]=MAX_UINT64;
    if(which==28){mScratch.Parent=(VOID*)1;mScratch.Scsi=(VOID*)1;}
    out[3]=code(mScratch.Io.ReadBlocks(&mScratch.Io,123,0,grant.BlockSize,buffer));
    out[4]=code(mScratch.Io.WriteBlocks(&mScratch.Io,123,grant.ScratchBlocks-1,grant.BlockSize,buffer));
    out[5]=code(mScratch.Io.FlushBlocks(&mScratch.Io));
    out[6]=code(mScratch.Io.ReadBlocks(&mScratch.Io,123,grant.ScratchBlocks,grant.BlockSize,buffer));
    out[7]=code(mScratch.Io.WriteBlocks(&mScratch.Io,123,0,grant.BlockSize-1,buffer));
    out[8]=code(mScratch.Io.ReadBlocks(&mScratch.Io,124,0,grant.BlockSize,buffer));
    out[9]=code(mScratch.Io.ReadBlocks(&mScratch.Io,123,0,grant.BlockSize,(UINT8*)buffer+1));
    out[10]=code(mScratch.Io.WriteBlocks(&mScratch.Io,123,0,0,NULL));
    out[11]=code(mScratch.Io.Reset(&mScratch.Io,FALSE));
    tpl=TPL_APPLICATION;
    out[12]=mScratch.Media.BlockSize;out[13]=mScratch.Media.LastBlock;out[14]=mScratch.Media.ReadOnly;
    out[15]=code(DaliUfsScratchUnpublish());out[16]=removes;out[17]=mScratch.Sense!=NULL;
    // A retained, closed child must not dereference a disconnected SCSI parent.
    mScratch.Parent=NULL;mScratch.Scsi=NULL;
    out[18]=code(mScratch.Io.ReadBlocks(&mScratch.Io,123,0,grant.BlockSize,buffer));
    free(buffer);
  }
  out[20]=submits;out[21]=policy_errors;out[22]=code(reentrant);out[23]=layout_reads;
  for(unsigned i=0;i<4;i++){out[24+i]=opcodes[i];out[28+i]=lbas[i];out[32+i]=lengths[i];}
  mode=0;out[36]=code(DaliUfsScratchUnpublish());out[37]=code(DaliUfsScratchUnpublish());
  out[38]=allocated;out[39]=freed;
}
