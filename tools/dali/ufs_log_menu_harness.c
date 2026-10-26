#include <string.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DaliUfsLog.h>
#include "DaliUfsLogMenu.h"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gDaliUfsLogProtocolGuid={1};
static unsigned mode,at,calls,selected,stalls,printed;
static EFI_STATUS EFIAPI run_log(DALI_UFS_LOG_PROTOCOL *self,UINT32 choice){(void)self;++calls;selected=choice;return mode==9?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static DALI_UFS_LOG_PROTOCOL protocol={DALI_UFS_LOG_REVISION,run_log};
static EFI_STATUS EFIAPI locate(EFI_GUID *guid,VOID *registration,VOID **out){
 (void)registration;if(guid!=&gDaliUfsLogProtocolGuid)return EFI_NOT_FOUND;
 *out=mode==7?NULL:&protocol;return mode==5?EFI_NOT_FOUND:mode==10?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,CHAR16 *text){
 (void)self;(void)text;++printed;return (mode==11&&printed==1)||(mode==12&&printed==2)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,EFI_INPUT_KEY *out){
 (void)self;memset(out,0,sizeof *out);unsigned i=at++;
 if(mode==3){if(i==0){out->ScanCode=SCAN_UP;return EFI_SUCCESS;}return EFI_NOT_READY;}
 if(mode==1||mode==12||mode==15){if(i==0)out->ScanCode=SCAN_UP;else if(i==1){if(mode==15)out->ScanCode=0x0102;else out->UnicodeChar=13;}else return EFI_NOT_READY;return EFI_SUCCESS;}
 if(mode==2){if(i==0)out->ScanCode=SCAN_UP;else if(i==1)out->ScanCode=SCAN_DOWN;else if(i==2)out->UnicodeChar=13;else return EFI_NOT_READY;return EFI_SUCCESS;}
 if(mode==4){if(i<3){out->ScanCode=0xffff;return EFI_SUCCESS;}return EFI_NOT_READY;}
 if(i==0){if(mode==16)out->ScanCode=0x0102;else out->UnicodeChar=13;return EFI_SUCCESS;}return EFI_NOT_READY;
}
static EFI_STATUS EFIAPI stall(UINTN us){if(us!=10000)return EFI_INVALID_PARAMETER;++stalls;return EFI_SUCCESS;}
VOID run_menu(unsigned fault,UINT64 *out){
 mode=fault;at=calls=selected=stalls=printed=0;protocol.Revision=mode==6?0:DALI_UFS_LOG_REVISION;protocol.Run=mode==8?NULL:run_log;
 EFI_BOOT_SERVICES bs={.LocateProtocol=locate,.Stall=stall};gBS=&bs;
 EFI_SIMPLE_TEXT_INPUT_PROTOCOL input={.ReadKeyStroke=key};EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL text={.OutputString=output};
 out[0]=DaliUfsLogMenu(mode==13?NULL:&input,mode==14?NULL:&text)&0x7fffffffffffffffULL;
 out[1]=calls;out[2]=selected;out[3]=stalls;out[4]=printed;
}
