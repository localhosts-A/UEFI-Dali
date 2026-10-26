// Native execution of the complete RAM consumer and provenance core.
#include <stdlib.h>
#include <string.h>
#include "UfsRamMeasurement.c"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiFileInfoGuid={1},gEfiSimpleFileSystemProtocolGuid={2},gEfiDevicePathProtocolGuid={3},gEfiBlockIoProtocolGuid={4};
static unsigned mode,reads,infos,hashes,closed_file,closed_root,allocated,freed;
static UINTN position,kernel_bytes;
static CONST UINT8 *kernel;
static BOOLEAN (*backend)(CONST VOID *,UINTN,UINT8 *);
static EFI_BLOCK_IO_MEDIA media;
static EFI_BLOCK_IO_PROTOCOL block;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static EFI_FILE_PROTOCOL root,file;
static UINT8 paths[2][24];
VOID set_backend(BOOLEAN (*b)(CONST VOID *,UINTN,UINT8 *)){backend=b;}
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n){return memcpy(d,s,n);}
VOID *EFIAPI ZeroMem(VOID *d,UINTN n){return memset(d,0,n);}
INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n){return memcmp(a,b,n);}
VOID *EFIAPI AllocatePool(UINTN n){if(mode==15&&allocated>=1)return NULL;VOID *p=malloc(n);if(p)++allocated;return p;}
VOID EFIAPI FreePool(VOID *p){++freed;free(p);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *p,UINTN n,UINT8 *out){
 ++hashes;if(closed_file!=1||closed_root!=1)abort();
 if((mode==20&&hashes==1)||(mode==21&&hashes==2)){memset(out,0xff,32);return FALSE;}
 return backend(p,n,out);
}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **handles){
 (VOID)type;(VOID)key;if(guid!=&gEfiSimpleFileSystemProtocolGuid)abort();
 *count=mode==1?2:1;*handles=AllocatePool(*count*sizeof **handles);if(!*handles)return EFI_OUT_OF_RESOURCES;
 (*handles)[0]=(VOID*)1;if(*count==2)(*handles)[1]=(VOID*)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocol(EFI_HANDLE h,EFI_GUID *guid,VOID **p){
 if(h!=(VOID*)1&&h!=(VOID*)2)abort();
 if(guid==&gEfiDevicePathProtocolGuid){*p=paths[h==(VOID*)1?0:1];return EFI_SUCCESS;}
 if(guid==&gEfiBlockIoProtocolGuid){*p=&block;return EFI_SUCCESS;}
 if(guid==&gEfiSimpleFileSystemProtocolGuid){*p=&fs;return EFI_SUCCESS;}
 return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **out){
 if(self!=&fs)abort();if(mode==5)return EFI_DEVICE_ERROR;*out=&root;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI open_file(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 access,UINT64 attr){
 static CONST CHAR16 expected[]=L"\\EFI\\UniXNU\\Payloads\\kernel.macho";
 if(self!=&root||access!=EFI_FILE_MODE_READ||attr||memcmp(name,expected,sizeof expected))abort();
 if(mode==6)return EFI_NOT_FOUND;*out=&file;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI info(EFI_FILE_PROTOCOL *self,EFI_GUID *guid,UINTN *bytes,VOID *data){
 if(self!=&file||guid!=&gEfiFileInfoGuid||*bytes<sizeof(EFI_FILE_INFO)+32)abort();++infos;
 if(mode==7)return EFI_DEVICE_ERROR;if(mode==8)return EFI_WARN_UNKNOWN_GLYPH;
 EFI_FILE_INFO *i=data;*bytes=SIZE_OF_EFI_FILE_INFO+2;memset(data,0,*bytes);i->Size=*bytes;i->FileSize=kernel_bytes;
 if(mode==9)i->Attribute=EFI_FILE_DIRECTORY;
 if(mode==10)i->FileSize=DALI_UFS_KERNEL_MAX_BYTES+1ULL;
 if(mode==11&&infos==2)++i->FileSize;
 if(mode==22)i->Size=0;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI seek(EFI_FILE_PROTOCOL *self,UINT64 at){if(self!=&file||at)abort();position=at;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI read_file(EFI_FILE_PROTOCOL *self,UINTN *n,VOID *data){
 if(self!=&file)abort();++reads;if(mode==12&&reads==1)return EFI_TIMEOUT;
 if(mode==13&&reads==1)return EFI_WARN_UNKNOWN_GLYPH;
 UINTN count=kernel_bytes-position;if(count>*n)count=*n;
 if(mode==14&&reads==1&&count)--count;
 memcpy(data,kernel+position,count);position+=count;*n=count;
 if(mode==16&&reads==2){((UINT8*)data)[0]=1;*n=1;}
 if(mode==17&&reads==1)++media.MediaId;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_file(EFI_FILE_PROTOCOL *self){
 if(self==&file){++closed_file;return mode==18?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 if(self==&root){++closed_root;return mode==19?EFI_DEVICE_ERROR:EFI_SUCCESS;}abort();
}
VOID run(unsigned fault,CONST UINT8 *k,UINTN n,CONST UINT8 *source,UINTN sn,UINT64 *out,UINT8 *digests){
 mode=fault;reads=infos=hashes=closed_file=closed_root=allocated=freed=0;kernel=k;kernel_bytes=n;position=0;
 memcpy(paths[0],&RamPath,24);memcpy(paths[1],&RamPath,24);if(mode==2)paths[0][4]^=1;
 media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.MediaPresent=TRUE,.ReadOnly=TRUE,.LogicalPartition=TRUE,.BlockSize=512,.LastBlock=49151};
 if(mode==3)media.ReadOnly=FALSE;if(mode==4)media.MediaPresent=FALSE;
 block=(EFI_BLOCK_IO_PROTOCOL){.Media=&media};fs=(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL){.OpenVolume=volume};
 root=(EFI_FILE_PROTOCOL){.Open=open_file,.Close=close_file};file=(EFI_FILE_PROTOCOL){.Read=read_file,.GetInfo=info,.SetPosition=seek,.Close=close_file};
 EFI_BOOT_SERVICES services={.LocateHandleBuffer=locate,.HandleProtocol=protocol};gBS=&services;
 DALI_UFS_PROVENANCE result;memset(&result,0xff,sizeof result);
 out[0]=DaliUfsMeasureRamPayload(source,sn,&result)&0x7fffffffffffffffULL;
 out[1]=result.Ready;out[2]=reads;out[3]=infos;out[4]=hashes;out[5]=closed_file;out[6]=closed_root;out[7]=allocated;out[8]=freed;
 memcpy(digests,result.CandidateSha256,32);memcpy(digests+32,result.SourceSha256,32);
}
