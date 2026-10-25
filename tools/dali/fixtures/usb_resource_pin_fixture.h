// Actual resource DXE's MMIO callback, guard/TPL/EBS behavior; registers modeled.
#pragma once
static UINT32 pin_regs[9];
static UINTN pin_reads,pin_guard_calls,pin_guard_fail,pin_ebs_guard,pin_ebs_read;
static BOOLEAN pin_reenter,pin_change;
static const UINTN pin_addresses[]={0x1002d3d0,0x13110020,0x13110050,0x131100c0,0x13110090,0x13110080,0x13110000,0x13110010,0x131100b0};
static UINT32 pin_mmio_read(UINTN address) {
  assert(current_tpl==TPL_APPLICATION && cpu_mapped[5] && cpu_mapped[6] && !mContext.Status.BootServicesClosed);
  unsigned index=9;
  for(unsigned n=0;n<9;n++) if(address==pin_addresses[n]) { index=n; break; }
  assert(index<9); pin_reads++;
  UINT32 value=pin_regs[index];
  if(pin_change && pin_reads==9) pin_regs[8]^=1u<<6;
  if(pin_ebs_read==pin_reads) { ExitBootServices(NULL,NULL); PmicExitBootServices(NULL,NULL); }
  return value;
}
static void pin_reset(void) {
  memset(pin_regs,0,sizeof pin_regs); pin_regs[0]=0xa5a50011; pin_regs[1]=(7u<<6)|(3u<<15);
  pin_regs[2]=1u<<11; pin_regs[7]=4u<<3; pin_regs[8]=(2u<<6)|(5u<<15);
  pin_reads=pin_guard_calls=pin_guard_fail=pin_ebs_guard=pin_ebs_read=0; pin_reenter=pin_change=FALSE;
}
static EFI_STATUS EFIAPI pin_guard(VOID *context) {
  assert(context==&pin_regs && current_tpl==TPL_APPLICATION); pin_guard_calls++;
  if(pin_reenter) {
    pin_reenter=FALSE; struct dali_i2c6_pin_snapshot out; DALI_USB_VUSB_TOKEN token={0}; DALI_USB_VUSB_INFO info;
    assert(mProtocol.ReadI2cPins(&mProtocol,pin_guard,context,&out)==EFI_NOT_READY);
    assert(mProtocol.AcquireVusb(&mProtocol,&token,power_guard,&power_guard_ok,&info)==EFI_NOT_READY && !token.Active);
  }
  if(pin_ebs_guard==pin_guard_calls) { ExitBootServices(NULL,NULL); PmicExitBootServices(NULL,NULL); }
  return pin_guard_fail==pin_guard_calls?EFI_TIMEOUT:EFI_SUCCESS;
}
static void pin_failure(EFI_STATUS expected) {
  struct dali_i2c6_pin_snapshot out,old; memset(&out,0xa5,sizeof out); old=out;
  assert(mProtocol.ReadI2cPins(&mProtocol,pin_guard,&pin_regs,&out)==expected && !memcmp(&out,&old,sizeof out));
}
static void pin_protocol_tests(void) {
  reset(); entry(EFI_SUCCESS); assert(pin_reads==0 && power_commands==0);
  DALI_USB_I2C_RESOURCES resources;
  assert(mProtocol.GetI2cResources(&mProtocol,&resources)==EFI_SUCCESS && resources.Gpio.Base==0x1002d000 &&
    resources.IocfgLb2.Base==0x13110000 && resources.Controller.Size==0x20000 && resources.Dma.Size==0x10000 &&
    resources.SclPin==104 && resources.SdaPin==105 && resources.ResetPin==72 && pin_reads==0);
  struct dali_i2c6_pin_snapshot out;
  pin_reenter=TRUE; assert(mProtocol.ReadI2cPins(&mProtocol,pin_guard,&pin_regs,&out)==EFI_SUCCESS && pin_reads==18 && pin_guard_calls==2 &&
    out.scl.mode==1 && out.sda.mode==1 && out.scl.enhanced_drive==7 && out.sda.enhanced_drive==3 &&
    out.scl.input_enable==1 && out.sda.input_enable==0 && out.sda.drive==4 && out.scl.resistance==2 && out.sda.resistance==5 && power_commands==0);
  reset(); entry(EFI_SUCCESS); current_tpl=TPL_CALLBACK; pin_failure(EFI_UNSUPPORTED); assert(pin_reads==0 && pin_guard_calls==0); current_tpl=TPL_APPLICATION;
  assert(mProtocol.ReadI2cPins(NULL,pin_guard,&pin_regs,&out)==EFI_INVALID_PARAMETER &&
    mProtocol.ReadI2cPins(&mProtocol,NULL,&pin_regs,&out)==EFI_INVALID_PARAMETER &&
    mProtocol.ReadI2cPins(&mProtocol,pin_guard,&pin_regs,NULL)==EFI_INVALID_PARAMETER && pin_reads==0);
  for(unsigned when=1;when<=2;when++) {
    reset(); entry(EFI_SUCCESS); pin_guard_fail=when; pin_failure(EFI_TIMEOUT); assert(pin_reads==(when==1?0:18));
    pin_guard_fail=0; assert(mProtocol.ReadI2cPins(&mProtocol,pin_guard,&pin_regs,&out)==EFI_SUCCESS);
    reset(); entry(EFI_SUCCESS); pin_ebs_guard=when; pin_failure(EFI_UNSUPPORTED); assert(pin_reads==(when==1?0:18));
  }
  for(unsigned when=1;when<=18;when++) {
    reset(); entry(EFI_SUCCESS); pin_ebs_read=when; pin_failure(EFI_UNSUPPORTED); assert(pin_reads==when && mContext.Status.BootServicesClosed);
    UINTN count=pin_reads; pin_failure(EFI_UNSUPPORTED); assert(pin_reads==count && !mVusb.Busy);
    DALI_USB_I2C_RESOURCES old; memset(&resources,0xa5,sizeof resources); old=resources;
    assert(mProtocol.GetI2cResources(&mProtocol,&resources)==EFI_UNSUPPORTED && !memcmp(&resources,&old,sizeof resources));
  }
  reset(); entry(EFI_SUCCESS); pin_change=TRUE; pin_failure(EFI_NOT_READY); assert(pin_reads==18);
  pin_change=FALSE; assert(mProtocol.ReadI2cPins(&mProtocol,pin_guard,&pin_regs,&out)==EFI_SUCCESS);
  reset(); entry(EFI_SUCCESS); pin_regs[0]=0x10; pin_failure(EFI_UNSUPPORTED); assert(power_commands==0);
  puts("PASS: installed read-only native I2C6 pin inspection, actual register decoder/DXE, modeled guard/reentry/TPL/EBS; no electrical or bus lease qualification");
}
