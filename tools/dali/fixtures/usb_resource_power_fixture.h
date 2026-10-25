// Actual PMIF DXE arbitration + native SID5 adapter/core; hardware/TPL model.
#pragma once
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#define mProtocol PmicProtocol
#define ExitBootServices PmicExitBootServices
#include "../../../Platforms/Xiaomi/daliPkg/Drivers/PmicBusDxe/PmicBusDxe.c"
#undef mProtocol
#undef ExitBootServices
EFI_GUID gDaliPmicBusProtocolGuid={.Data1=3};
static uint8_t power5[0x2000],power14[0x1000];
static UINT32 power_sta,power_data,power_write;
static UINTN power_commands,power_writes,power_guard_calls,power_stuck_command,power_time;
static BOOLEAN power_bus_missing,power_guard_ok,power_guard_reenter,power_ebs_guard,power_ebs_restore,power_error_enable;
static DALI_USB_VUSB_TOKEN *power_test_token;
static EFI_STATUS EFIAPI power_guard(VOID *context);
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *value,UINT32 compare,UINT32 exchange) {
  UINT32 old=*value; if(old==compare) *value=exchange; return old;
}
VOID EFIAPI MemoryFence(VOID) {}
UINT32 EFIAPI MmioRead32(UINTN address) {
  if(address<0x1c01a000 || address>=0x1c01a8ff) return native_bus_enabled()?native_bus_read32(address):pin_mmio_read(address);
  assert(current_tpl==TPL_HIGH_LEVEL && address>=0x1c01a000 && address<0x1c01a8ff);
  UINTN offset=address-0x1c01a000;
  if(offset==0) return 1;
  if(offset==0x8a8) return power_sta;
  assert(offset==0x894); return power_data;
}
UINT32 EFIAPI MmioWrite32(UINTN address,UINT32 value) {
  if(address<0x1c01a000 || address>=0x1c01a8ff) return native_bus_write32(address,value);
  assert(current_tpl==TPL_HIGH_LEVEL && address>=0x1c01a000 && address<0x1c01a8ff);
  UINTN offset=address-0x1c01a000;
  if(offset==0x884) power_write=value;
  else if(offset==0x8a4) { assert(value==1); power_sta=0; }
  else {
    assert(offset==0x880); power_commands++;
    unsigned sid=(value>>24)&15, count=((value>>16)&255)+1, reg=value&0xffff;
    assert((sid==5 || sid==14) && count<=2);
    uint8_t *bytes=sid==5?power5:power14;
    assert(reg+count<=(sid==5?sizeof power5:sizeof power14));
    if(value&(1U<<29)) {
      assert(sid==5 && reg==0x1b95 && count==1 && (power_write&1) &&
        (uint8_t)(power_write&~1U)==(uint8_t)(bytes[reg]&~1U));
      bytes[reg]=(uint8_t)power_write; power_writes++; power_sta=power_error_enable?1U<<18:0;
    } else {
      power_data=0; for(unsigned i=0;i<count;i++) power_data|=(UINT32)bytes[reg+i]<<(8*i);
      power_sta=power_commands==power_stuck_command?2:12;
    }
  }
  return value;
}
UINTN EFIAPI MicroSecondDelay(UINTN us) { if(native_bus_enabled()) native_bus_delay(us); else power_time+=us; return us; }
static EFI_STATUS EFIAPI power_locate_bus(EFI_GUID *guid,VOID *registration,VOID **out) {
  (VOID)registration; assert(guid==&gDaliPmicBusProtocolGuid && current_tpl==TPL_APPLICATION);
  if(power_bus_missing) return EFI_NOT_FOUND;
  *out=&PmicProtocol; return EFI_SUCCESS;
}
static void power_on_restore(void) {
  if(power_ebs_restore && current_tpl==TPL_APPLICATION && power_commands>0) {
    power_ebs_restore=FALSE; ExitBootServices(NULL,NULL); PmicExitBootServices(NULL,NULL);
  }
}
static void power_reset(void) {
  pin_reset(); native_bus_reset();
  memset(&mVusb,0,sizeof mVusb); mBusy=0; mClosing=FALSE; mOwner=mReserved=NULL;
  memset(power5,0,sizeof power5); memset(power14,0,sizeof power14);
  power5[8]=0x10; power5[9]=0x73; power5[0xb]=0x10;
  power5[0x1b95]=0xa2; power5[0x1e0c]=0xa7; power5[0x1e0d]=0xcd; power14[0]=0x72;
  power_commands=power_writes=power_guard_calls=power_stuck_command=power_time=0;
  power_bus_missing=power_guard_reenter=power_ebs_guard=power_ebs_restore=power_error_enable=FALSE;
  power_guard_ok=TRUE; power_test_token=NULL;
}
static EFI_STATUS EFIAPI power_guard(VOID *context) {
  assert(context==&power_guard_ok && current_tpl==TPL_APPLICATION); power_guard_calls++;
  if(power_guard_reenter) {
    power_guard_reenter=FALSE; DALI_USB_VUSB_TOKEN other={0}; DALI_USB_VUSB_INFO info;
    assert(mProtocol.AcquireVusb(&mProtocol,&other,power_guard,context,&info)==EFI_NOT_READY && !other.Active);
    assert(mProtocol.ReleaseVusb(&mProtocol,power_test_token)==EFI_NOT_READY);
  }
  if(power_ebs_guard) { power_ebs_guard=FALSE; ExitBootServices(NULL,NULL); PmicExitBootServices(NULL,NULL); }
  return power_guard_ok?EFI_SUCCESS:EFI_NOT_READY;
}
static EFI_STATUS power_acquire(DALI_USB_VUSB_TOKEN *token,DALI_USB_VUSB_INFO *info) {
  power_test_token=token; return mProtocol.AcquireVusb(&mProtocol,token,power_guard,&power_guard_ok,info);
}
static void power_failed_output(DALI_USB_VUSB_TOKEN *token,EFI_STATUS expected) {
  DALI_USB_VUSB_INFO info,old; memset(&info,0xa5,sizeof info); old=info;
  assert(power_acquire(token,&info)==expected && !memcmp(&info,&old,sizeof info));
}
static void power_protocol_tests(void) {
  reset(); entry(EFI_SUCCESS); assert(power_commands==0 && mProtocol.Revision==5);
  DALI_USB_VUSB_RESOURCES desc; assert(mProtocol.GetVusbResources(&mProtocol,&desc)==EFI_SUCCESS && desc.SpmiDeviceMask==0x85c0);
  DALI_USB_VUSB_TOKEN a={0},b={0}; DALI_USB_VUSB_INFO info;
  current_tpl=TPL_NOTIFY; power_failed_output(&a,EFI_UNSUPPORTED); assert(!a.Active && !power_commands);
  current_tpl=TPL_APPLICATION;
  a.Active=TRUE; power_failed_output(&a,EFI_INVALID_PARAMETER); assert(!power_commands); a.Active=FALSE;
  power_guard_ok=FALSE; power_failed_output(&a,EFI_NOT_READY); assert(a.Active && power_commands==0);
  power_failed_output(&b,EFI_NOT_READY); assert(!b.Active && power_commands==0);
  assert(mProtocol.ReleaseVusb(&mProtocol,&b)==EFI_ACCESS_DENIED);
  assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_NOT_READY && a.Active);
  power_guard_ok=TRUE; assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_SUCCESS && !a.Active && power_commands==0);
  power_guard_reenter=TRUE; assert(power_acquire(&a,&info)==EFI_SUCCESS && a.Active && power_writes==1 && power_time>=720);
  assert(info.NominalMicrovolt==3300000 && info.Selector==13 && info.Trim==7 && info.LowPower==2 && info.ChipRevision==0x10);
  assert(current_tpl==TPL_APPLICATION && !mBusy && !mReserved && !mVusb.Pmic.Token.Active && power5[0x1b95]==0xa3);
  UINTN commands=power_commands; assert(mProtocol.AcquireVusb(&mProtocol,&a,power_guard,NULL,&info)==EFI_ACCESS_DENIED && commands==power_commands);
  assert(power_acquire(&a,&info)==EFI_SUCCESS && power_writes==1);
  assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_SUCCESS && !a.Active && power5[0x1b95]==0xa3 && power_writes==1);
  assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_ACCESS_DENIED);

  reset(); entry(EFI_SUCCESS); a=(DALI_USB_VUSB_TOKEN){0}; power5[9]=0x79;
  power_failed_output(&a,EFI_UNSUPPORTED); assert(a.Active && power_writes==0);
  assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_SUCCESS && !a.Active);
  reset(); entry(EFI_SUCCESS); a=(DALI_USB_VUSB_TOKEN){0}; power_bus_missing=TRUE;
  power_failed_output(&a,EFI_NOT_FOUND); assert(a.Active && !power_commands);
  power_bus_missing=FALSE; assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_SUCCESS && !a.Active);
  reset(); entry(EFI_SUCCESS); a=(DALI_USB_VUSB_TOKEN){0}; power_error_enable=TRUE;
  power_failed_output(&a,EFI_DEVICE_ERROR); assert(a.Active && power_writes==1 && (power5[0x1b95]&1));
  power_failed_output(&a,EFI_DEVICE_ERROR); assert(power_writes==1);
  power_error_enable=FALSE; assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_SUCCESS && !a.Active && power_writes==1);

  reset(); entry(EFI_SUCCESS); a=(DALI_USB_VUSB_TOKEN){0}; power_stuck_command=1;
  power_failed_output(&a,EFI_TIMEOUT); assert(a.Active && mReserved==&mVusb.Pmic.Token && mVusb.Pmic.Transport.pending_owned);
  DALI_USB_PMIC peer={0}; assert(DaliUsbPmicOpen(&peer,tree)==EFI_NOT_READY && !peer.Transport.initialized);
  power_stuck_command=0; power_sta=12;
  assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_SUCCESS && !mReserved && !a.Active);
  assert(DaliUsbPmicOpen(&peer,tree)==EFI_SUCCESS && peer.Transport.sid==14 && peer.Ready);
  assert(DaliUsbPmicClose(&peer)==EFI_SUCCESS);

  for(unsigned when=0;when<3;when++) {
    reset(); entry(EFI_SUCCESS); a=(DALI_USB_VUSB_TOKEN){0};
    if(when==0) power_ebs_guard=TRUE;
    else if(when==1) power_ebs_restore=TRUE;
    else { assert(power_acquire(&a,&info)==EFI_SUCCESS); ExitBootServices(NULL,NULL); PmicExitBootServices(NULL,NULL); }
    power_failed_output(&a,EFI_UNSUPPORTED); assert(a.Active && mVusb.Owner==&a && mContext.Status.BootServicesClosed);
    commands=power_commands;
    assert(mProtocol.ReleaseVusb(&mProtocol,&a)==EFI_UNSUPPORTED && a.Active);
    assert(power_commands==commands && current_tpl==TPL_APPLICATION);
    DALI_USB_VUSB_RESOURCES old; memset(&desc,0xa5,sizeof desc); old=desc;
    assert(mProtocol.GetVusbResources(&mProtocol,&desc)==EFI_UNSUPPORTED && !memcmp(&desc,&old,sizeof desc));
  }
  puts("PASS: installed native VUSB protocol, actual SID5/SID14 PMIF/lease code, modeled hardware/ownership/reentry/errors/EBS; no source/power-good or hardware qualification");
}
