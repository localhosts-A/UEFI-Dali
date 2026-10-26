// Native fault injection around production UfsLog and actual grant policy.
#include <stdlib.h>
#include <string.h>
#include "UfsLog.h"
static BOOLEAN (*backend)(CONST VOID *,UINTN,UINT8 *);
static UINT8 *disk;
static UINT32 block;
static UINTN window;
static unsigned mode,reads,writes,syncs,hashes;
VOID set_log_backend(BOOLEAN (*b)(CONST VOID *,UINTN,UINT8 *)) { backend=b; }
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n) { return memcpy(d,s,n); }
VOID *EFIAPI ZeroMem(VOID *d,UINTN n) { return memset(d,0,n); }
INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n) { return memcmp(a,b,n); }
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *p,UINTN n,UINT8 *out) {
  if(mode==9&&++hashes==2)return FALSE;return backend(p,n,out);
}
EFI_STATUS Mt6991UfsDmaResolve(MT6991_UFS_DMA *d,UINT64 a,UINTN n,BOOLEAN c,VOID **p) {
  (void)d;(void)a;(void)n;(void)c;(void)p;return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO_PROTOCOL *io,UINT32 id,EFI_LBA lba,UINTN n,VOID *p) {
  if(id!=3||io->Media->MediaId!=3||lba*block+n>window)abort();++reads;
  if((mode==1&&reads==1)||(mode==7&&reads==2))return EFI_TIMEOUT;
  memcpy(p,disk+lba*block,n);if(mode==8&&reads==2)((UINT8*)p)[2*block]^=1;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_blocks(EFI_BLOCK_IO_PROTOCOL *io,UINT32 id,EFI_LBA lba,UINTN n,VOID *p) {
  if(id!=3||io->Media->MediaId!=3||lba*block+n>window)abort();++writes;
  if(mode==2&&writes==1){memcpy(disk+lba*block,p,n/2);return EFI_TIMEOUT;}
  if(mode==3&&writes==2){memcpy(disk+lba*block,p,n/2);return EFI_DEVICE_ERROR;}
  memcpy(disk+lba*block,p,n);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_BLOCK_IO_PROTOCOL *io) {
  (void)io;++syncs;return mode>=4&&mode<=6&&syncs==mode-3?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
VOID run_log(unsigned fault,unsigned block_bytes,BOOLEAN readback,unsigned owner,UINT64 sequence,
  UINT8 *media,CONST UINT8 *body,UINTN bytes,UINT64 *out,UINT8 *digest) {
  mode=fault;block=block_bytes;disk=media;reads=writes=syncs=hashes=0;
  BOOLEAN userdata=owner>=10;if(userdata)owner-=10;
  window=userdata?DALI_UFS_LOG_USERDATA_SLOT_BYTES:DALI_UFS_LOG_WINDOW_BYTES;
  UINT64 sequenceSlot=userdata?(sequence-1)*2*(MT6991_UFS_DATA_LOG_SLOT_BYTES/4096):0;
  MT6991_UFS_SCRATCH_GRANT g={.Enabled=TRUE,.Lun=2,.BlockSize=block,.CapacityBlocks=64577536,
    .BootStartLba=userdata?64565240:2184,.BootBlocks=userdata?8192:134217728/block,
    .ScratchStartLba=userdata?64565240+sequenceSlot:2184+1048576/block,
    .ScratchBlocks=window/block,.TailGuardBlocks=userdata?8192-sequenceSlot-window/block:132120576/block,
    .Sequence=sequence,.Target=userdata?MT6991_UFS_TARGET_USERDATA_LOGS:MT6991_UFS_TARGET_EXPDB};
  if(userdata){const UINT8 guid[16]={0xcd,0xe9,0xab,0x32,0x18,0xd8,0x47,0x77,0x8c,0xed,0x69,0x1e,0x7c,0xd5,0xc6,0x36};memcpy(g.PartitionGuid,guid,16);}
  else {memset(g.PartitionGuid,4,16);}
  memset(g.Token,'a',32);memset(g.DeviceBinding,1,32);memset(g.CandidateSha256,2,32);
  memset(g.SourceSha256,3,32);
  EFI_BLOCK_IO_MEDIA m={.MediaId=3,.MediaPresent=TRUE,.LogicalPartition=TRUE,.ReadOnly=mode==10,
    .BlockSize=block,.LastBlock=window/block-1,.IoAlign=1};
  if(mode==11)--m.LastBlock;
  if(mode==12)g.Target=MT6991_UFS_TARGET_BOOT_B;
  if(mode==13)g.Token[0]='b';
  EFI_BLOCK_IO_PROTOCOL io={.Media=&m,.ReadBlocks=read_blocks,.WriteBlocks=write_blocks,.FlushBlocks=flush};
  UINT8 *work=malloc(DALI_UFS_LOG_WORK_BYTES);if(work==NULL)abort();
  DALI_UFS_LOG_RESULT result={0};
  out[0]=DaliUfsLogRun(&io,&g,owner,sequence,readback,body,bytes,work,DALI_UFS_LOG_WORK_BYTES,&result)&0x7fffffffffffffffULL;
  out[1]=reads;out[2]=writes;out[3]=syncs;out[4]=result.Attempted;out[5]=result.WriteSubmitted;
  out[6]=result.CommitSynced;out[7]=result.Verified;out[8]=result.BodyBytes;
  memcpy(digest,result.BodySha256,32);
  if(result.Verified&&memcmp(result.Body,body?body:disk+2*block,result.BodyBytes))abort();
  out[9]=DaliUfsLogRun(&io,&g,owner,sequence,readback,body,bytes,work,DALI_UFS_LOG_WORK_BYTES,&result)&0x7fffffffffffffffULL;
  out[10]=reads;out[11]=writes;out[12]=syncs;
  free(work);
}
