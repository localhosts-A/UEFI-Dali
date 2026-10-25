// Actual native bus/library + NXP/core pipeline; borrowed controller register
// model and its full independent legacy regressions, not a production seam.
#pragma once
#define main I2cLegacyModelMain
#include DALI_I2C_FIXTURE_FILE
#undef main
static struct {
  struct model hw;
  UINT32 clock[5][0x1000/4];
  UINTN ops,ebs_at,guard_calls,guard_fail,policy_ebs,clock_writes,chip_writes,drop_gate,controller_ops;
  BOOLEAN enabled,applied,validate_ok,quiesce_ok,release_ok,reenter,delay_reenter,lose_after_write,delay_ebs;
  DALI_USB_I2C_TOKEN token;
  DALI_USB_I2C_POLICY_PHASE delay_phase;
  struct dali_nxp_repeater_bus bus;
} nb;
static BOOLEAN native_bus_enabled(void) { return nb.enabled; }
static void native_bus_reset(void) {
  memset(&nb,0,sizeof nb); memset(&mI2cBus,0,sizeof mI2cBus); mI2cOwner=NULL;
  prepare(&nb.hw); nb.validate_ok=nb.quiesce_ok=nb.release_ok=TRUE;
  nb.clock[0][0x90/4]=0x808000; nb.clock[1][0xb0/4]=0x80000000;
  nb.clock[1][0xc0/4]=1u<<16; nb.clock[1][0x10/4]=1u<<24;
  nb.clock[1][0x274/4]=0x20000; nb.clock[2][0x3c/4]=0x80; nb.clock[2][0x254/4]=0x140000;
  nb.clock[3][0xe00/4]=0xa5a50004; nb.clock[4][0x14/4]=0x04000080;
}
static void native_bus_ebs(void) {
  if(nb.ops&1) { PmicExitBootServices(NULL,NULL); ExitBootServices(NULL,NULL); }
  else { ExitBootServices(NULL,NULL); PmicExitBootServices(NULL,NULL); }
}
static BOOLEAN native_before(void) {
  assert(nb.enabled && current_tpl==TPL_APPLICATION && !mI2cBus.Closed);
  nb.ops++;
  if(nb.ebs_at==nb.ops && !nb.applied) { native_bus_ebs(); return FALSE; }
  return TRUE;
}
static void native_after(void) { if(nb.ebs_at==nb.ops && nb.applied) native_bus_ebs(); }
static void native_controller_access(void) {
  assert(!(nb.clock[3][0xe00/4]&4) && !(nb.clock[4][0x14/4]&(1u<<26)));
  nb.controller_ops++;
}
static int clock_bank(UINTN address,UINT32 *offset) {
  const UINTN base[]={0x1c016000,0x10000000,0x10000800,0x131b0000,0x16640000};
  const UINTN length[]={0x1000,0x800,0x1000,0x1000,0x1000};
  for(int i=0;i<5;i++) if(address>=base[i] && address-base[i]<length[i]) { *offset=(UINT32)(address-base[i]); return i; }
  return -1;
}
static UINT32 native_bus_read32(UINTN address) {
  if(!native_before()) return 0;
  UINT32 value=0,offset; int bank_index=clock_bank(address,&offset);
  if(bank_index>=0) value=nb.clock[bank_index][offset/4];
  else if(address>=0x13170000 && address<0x13170100) {
    native_controller_access();
    offset=(UINT32)(address-0x13170000);
    if(offset==0xc && nb.hw.running && (nb.hw.c[0x10/4]&0x10)) {
      assert((nb.hw.output&0x100) && (nb.hw.direction&0x100) && nb.hw.time-nb.hw.high_time>=4000);
    }
    bool writing=nb.hw.running && !(nb.hw.c[0x10/4]&0x10);
    assert(ctrl_read(&nb.hw,offset,&value)==USB_OK);
    if(writing && !nb.hw.running) {
      nb.chip_writes++;
      if(nb.lose_after_write) {nb.lose_after_write=FALSE;nb.validate_ok=FALSE;}
    }
  } else if(address==0x16410008) { native_controller_access();assert(dma_read(&nb.hw,8,&value)==USB_OK); }
  else {
    BOOLEAN found=FALSE;
    for(unsigned n=0;n<9;n++) if(address==pin_addresses[n]) { value=pin_regs[n];found=TRUE;break; }
    if(!found) {
      assert(address>=0x1002d000 && address<0x1002e000);
      assert(gpio_read(&nb.hw,(UINT32)(address-0x1002d000),&value)==USB_OK);
    }
  }
  native_after(); return value;
}
static UINT32 native_bus_write32(UINTN address,UINT32 value) {
  if(!native_before()) return value;
  if(address==0x131b0e04 || address==0x131b0e08) {
    assert(value==4); nb.clock_writes++;
    if(address==0x131b0e04) { if(address!=nb.drop_gate) nb.clock[3][0xe00/4]&=~value; }
    else { assert(!nb.hw.running && !(nb.hw.d[8/4]&1));nb.clock[3][0xe00/4]|=value; }
  } else if(address==0x16640030 || address==0x1664002c) {
    assert(value==(1u<<26));nb.clock_writes++;
    if(address==0x16640030) { if(address!=nb.drop_gate) nb.clock[4][0x14/4]&=~value; }
    else { assert(!nb.hw.running && !(nb.hw.d[8/4]&1));nb.clock[4][0x14/4]|=value; }
  } else if(address>=0x13170000 && address<0x13170100) {
    native_controller_access();
    assert(ctrl_write(&nb.hw,(UINT32)(address-0x13170000),value)==USB_OK);
  } else if(address==0x1641000c || address==0x1641001c) {
    native_controller_access();
    assert(dma_write(&nb.hw,(UINT32)(address-0x16410000),value)==USB_OK);
  } else {
    assert(address>=0x1002d000 && address<0x1002e000);
    assert(gpio_write(&nb.hw,(UINT32)(address-0x1002d000),value)==USB_OK);
  }
  native_after(); return value;
}
UINT8 EFIAPI MmioRead8(UINTN address) {
  assert(address==0x13170000);
  if(!native_before()) return 0;
  native_controller_access();
  UINT8 value=0;assert(data_read(&nb.hw,0,&value)==USB_OK);native_after();return value;
}
UINT8 EFIAPI MmioWrite8(UINTN address,UINT8 value) {
  assert(address==0x13170000);
  if(!native_before()) return value;
  native_controller_access();
  assert(data_write(&nb.hw,0,value)==USB_OK);native_after();return value;
}
static void native_bus_delay(UINTN us) {
  nb.hw.time+=us;
  if(nb.delay_ebs) {nb.delay_ebs=FALSE;native_bus_ebs();}
}
static EFI_STATUS EFIAPI native_policy(VOID *context,DALI_USB_I2C_POLICY_PHASE phase) {
  assert(context==&nb && current_tpl==TPL_APPLICATION);nb.guard_calls++;
  if(nb.policy_ebs==nb.guard_calls) {native_bus_ebs();return EFI_SUCCESS;}
  if(nb.reenter) {
    nb.reenter=FALSE; UINT8 out=0xa5;DALI_USB_I2C_INFO info;DALI_USB_I2C_TOKEN other={0};struct dali_nxp_repeater_bus bus;
    assert(BusRead(&nb.token,0x15,&out)==USB_BUSY && out==0xa5);
    assert(mProtocol.GetI2cBusStatus(&mProtocol,&nb.token,&info)==EFI_NOT_READY);
    assert(mProtocol.AcquireI2cBus(&mProtocol,&other,native_policy,context,0,&bus,&info)==EFI_NOT_READY && !other.Active);
  }
  if(nb.delay_reenter && phase==nb.delay_phase) { nb.delay_reenter=FALSE;BusDelay(&nb.token,10); }
  if(nb.guard_calls==nb.guard_fail) return EFI_TIMEOUT;
  if(phase==DaliUsbI2cValidate) return nb.validate_ok?EFI_SUCCESS:EFI_NOT_READY;
  if(phase==DaliUsbI2cQuiesce) return nb.quiesce_ok?EFI_SUCCESS:EFI_NOT_READY;
  assert(phase==DaliUsbI2cRelease);
  return nb.release_ok && !nb.hw.nxp.initialized?EFI_SUCCESS:EFI_NOT_READY;
}
static int native_nxp_quiesce(VOID *context) { return UsbResult(native_policy(context,DaliUsbI2cQuiesce)); }
static EFI_STATUS native_open(DALI_USB_I2C_INFO *info) {
  return mProtocol.AcquireI2cBus(&mProtocol,&nb.token,native_policy,&nb,0,&nb.bus,info);
}
static void native_failed_open(EFI_STATUS expected) {
  DALI_USB_I2C_INFO info,old;memset(&info,0xa5,sizeof info);old=info;
  struct dali_nxp_repeater_bus oldbus;memset(&nb.bus,0xa5,sizeof nb.bus);oldbus=nb.bus;
  assert(native_open(&info)==expected && !memcmp(&info,&old,sizeof info) && !memcmp(&nb.bus,&oldbus,sizeof oldbus));
}
static void native_boot(void) { reset();nb.enabled=TRUE;entry(EFI_SUCCESS);assert(nb.ops==0 && !nb.hw.starts && !nb.hw.gpio_writes); }
static void native_probe(void) {
  struct dali_repeater_guard g={&nb,native_nxp_quiesce};
  assert(dali_nxp_repeater_probe(&nb.hw.nxp,&nb.bus,&g,true)==USB_OK && nb.hw.nxp.identified);
  assert(nb.hw.high_time-nb.hw.low_time==10 && nb.hw.time-nb.hw.high_time>=4000);
}
static void native_destroy(void) {
  assert(dali_nxp_repeater_destroy(&nb.hw.nxp)==USB_OK);
  assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_SUCCESS && !nb.token.Active && !mI2cOwner && !mI2cBus.Initialized);
  assert((nb.clock[3][0xe00/4]&4) && (nb.clock[4][0x14/4]&(1u<<26)) && !(nb.hw.output&0x100));
}
static void native_normal(void) {
  native_boot();DALI_USB_I2C_INFO info;nb.reenter=TRUE;
  assert(native_open(&info)==EFI_SUCCESS && info.Ready && info.OwnedGates==3 && info.MainHz==65000000 &&
    info.DmaHz==37142857 && !nb.hw.starts && !nb.hw.gpio_writes && nb.clock_writes==2);
  UINTN writes=nb.hw.writes,clocks=nb.clock_writes;
  assert(native_open(&info)==EFI_SUCCESS && nb.hw.writes==writes && nb.clock_writes==clocks);
  DALI_USB_I2C_TOKEN other={0};struct dali_nxp_repeater_bus bus;
  assert(mProtocol.AcquireI2cBus(&mProtocol,&other,native_policy,&nb,0,&bus,&info)==EFI_NOT_READY && !other.Active);
  assert(mProtocol.ReleaseI2cBus(&mProtocol,&other)==EFI_ACCESS_DENIED);
  assert(mProtocol.AcquireI2cBus(&mProtocol,&nb.token,native_policy,NULL,0,&bus,&info)==EFI_ACCESS_DENIED);
  native_probe();
  assert(dali_nxp_repeater_mode(&nb.hw.nxp,USB_ROLE_HOST)==USB_OK && nb.hw.chip[2]==0xdf);
  assert(dali_nxp_repeater_mode(&nb.hw.nxp,USB_ROLE_DEVICE)==USB_OK && nb.hw.chip[2]==0xef);
  struct dali_nxp_repeater_bus stale=nb.bus;native_destroy();UINT8 out=0xa5;
  assert(stale.read8(stale.context,0x15,&out)==USB_UNSUPPORTED && out==0xa5);
}
static void native_runtime_faults(void) {
  native_boot();DALI_USB_I2C_INFO info;assert(native_open(&info)==EFI_SUCCESS);native_probe();
  nb.hw.fail_slave_write=true;assert(dali_nxp_repeater_mode(&nb.hw.nxp,USB_ROLE_HOST)==USB_IO);
  assert(mProtocol.GetI2cBusStatus(&mProtocol,&nb.token,&info)==EFI_SUCCESS && info.Faulted && !info.Ready && info.WriteUncertain);
  unsigned starts=nb.hw.starts;assert(nb.bus.write8(nb.bus.context,2,0xef)==USB_IO && nb.hw.starts==starts);
  nb.hw.fail_slave_write=false;native_destroy();
  native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();nb.hw.timeout=true;
  assert(dali_nxp_repeater_mode(&nb.hw.nxp,USB_ROLE_HOST)==USB_TIMEOUT && mI2cBus.Faulted && !mI2cBus.WriteUncertain);
  nb.hw.timeout=false;native_destroy();
  native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();nb.hw.timeout=true;
  assert(nb.bus.write8(nb.bus.context,2,0xdf)==USB_TIMEOUT && mI2cBus.Faulted && mI2cBus.WriteUncertain);
  nb.hw.timeout=false;native_destroy();
  native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();nb.lose_after_write=TRUE;
  assert(dali_nxp_repeater_mode(&nb.hw.nxp,USB_ROLE_HOST)==USB_BUSY && nb.hw.chip[2]==0xdf && nb.chip_writes==1);
  assert(mProtocol.GetI2cBusStatus(&mProtocol,&nb.token,&info)==EFI_SUCCESS && info.Faulted && info.WriteUncertain);
  starts=nb.hw.starts;assert(nb.bus.write8(nb.bus.context,2,0xef)==USB_IO && nb.hw.starts==starts && nb.chip_writes==1);
  nb.validate_ok=TRUE;native_destroy();
  native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();
  assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_NOT_READY && nb.token.Active && mI2cBus.Faulted);
  native_destroy();
  for(unsigned what=0;what<4;what++) {
    native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();UINT8 out=0xa5;UINT32 old;
    if(what==0) {old=pin_regs[8];pin_regs[8]^=1u<<6;}
    else if(what==1) {old=nb.clock[1][0xc0/4];nb.clock[1][0xc0/4]^=1u<<16;}
    else if(what==2) {old=nb.hw.d[8/4];nb.hw.d[8/4]=1;}
    else {old=nb.hw.mode;nb.hw.mode=1;}
    unsigned resets=nb.hw.resets;
    assert(nb.bus.read8(nb.bus.context,0x15,&out)==(what==3?USB_UNSUPPORTED:USB_BUSY) && out==0xa5 && mI2cBus.Faulted);
    assert(dali_nxp_repeater_destroy(&nb.hw.nxp)!=USB_OK && nb.hw.nxp.initialized && nb.hw.resets==resets);
    if(what==0) pin_regs[8]=old;else if(what==1)nb.clock[1][0xc0/4]=old;else if(what==2)nb.hw.d[8/4]=old;else nb.hw.mode=old;
    native_destroy();
  }
  native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();
  nb.delay_reenter=TRUE;UINT8 out=0xa5;assert(nb.bus.read8(nb.bus.context,0x15,&out)==USB_IO && out==0xa5 && mI2cBus.Faulted);
  native_destroy();
  native_boot();assert(native_open(&info)==EFI_SUCCESS);native_probe();
  nb.delay_reenter=TRUE;nb.delay_phase=DaliUsbI2cQuiesce;unsigned gpio_writes=nb.hw.gpio_writes;
  assert(nb.bus.enable(nb.bus.context,true)==USB_IO && nb.hw.gpio_writes==gpio_writes && mI2cBus.Faulted);
  native_destroy();
}
static void native_startup_faults(void) {
  native_boot();DALI_USB_I2C_INFO info;assert(native_open(&info)==EFI_SUCCESS);UINTN total=nb.ops,guards=nb.guard_calls;
  for(UINTN at=1;at<=guards;at++) {
    native_boot();nb.guard_fail=at;native_failed_open(EFI_TIMEOUT);assert(nb.token.Active && mI2cBus.Faulted);
    nb.guard_fail=0;assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_SUCCESS && !nb.token.Active);
    native_boot();nb.policy_ebs=at;native_failed_open(EFI_UNSUPPORTED);assert(nb.token.Active && mI2cBus.Closed);
  }
  native_boot();current_tpl=TPL_NOTIFY;native_failed_open(EFI_UNSUPPORTED);assert(!nb.token.Active && nb.ops==0);
  current_tpl=TPL_APPLICATION;nb.token.Active=TRUE;native_failed_open(EFI_INVALID_PARAMETER);assert(nb.ops==0);
  for(unsigned what=0;what<6;what++) {
    native_boot();
    if(what==0) nb.hw.d[8/4]=1;else if(what==1)nb.hw.c[0x8c/4]=2;else if(what==2)nb.hw.c[0x24/4]=1;
    else if(what==3)nb.hw.c[8/4]=1;else if(what==4)nb.hw.c[0xc/4]=1;else nb.hw.rx_count=1;
    native_failed_open(what==1?EFI_UNSUPPORTED:EFI_NOT_READY);
    unsigned resets=nb.hw.resets;UINTN gate_writes=nb.clock_writes;
    assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==(what==1?EFI_UNSUPPORTED:EFI_NOT_READY) &&
      nb.token.Active && nb.hw.resets==resets && nb.clock_writes==gate_writes);
    nb.hw.d[8/4]=nb.hw.c[0x8c/4]=nb.hw.c[0x24/4]=nb.hw.c[8/4]=nb.hw.c[0xc/4]=nb.hw.rx_count=0;
    assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_SUCCESS && !nb.token.Active);
  }
  for(UINTN at=1;at<=total;at++) for(unsigned applied=0;applied<2;applied++) {
    native_boot();nb.ebs_at=at;nb.applied=(BOOLEAN)applied;native_failed_open(EFI_UNSUPPORTED);
    assert(nb.ops==at && nb.token.Active && mI2cBus.Initialized && mI2cBus.Closed && mI2cOwner==&nb.token && !mVusb.Busy);
    assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_UNSUPPORTED && nb.ops==at);
  }
  printf("PASS: native I2C startup EBS before/applied effects at %llu MMIO positions,%llu guard failures; no physical qualification\n",
    (unsigned long long)total,(unsigned long long)guards);
}
static void native_partial_clocks(void) {
  const UINTN address[]={0x131b0e04,0x16640030};
  for(unsigned gate=0;gate<2;gate++) {
    native_boot();nb.drop_gate=address[gate];native_failed_open(EFI_TIMEOUT);
    assert(nb.token.Active && mI2cBus.Faulted && !mI2cBus.Clocks.on &&
      mI2cBus.Clocks.owned==(gate?3:1) && !nb.controller_ops && !nb.hw.gpio_writes);
    nb.quiesce_ok=FALSE;nb.hw.d[8/4]=1;UINTN writes=nb.clock_writes;
    assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_NOT_READY &&
      nb.token.Active && nb.clock_writes==writes && !nb.controller_ops && mI2cBus.Clocks.owned==(gate?3:1));
    // External policy completes its stop first; no unclocked engine read may
    // be used to replace that proof, even while cleanup restores owned gates.
    nb.hw.d[8/4]=0;nb.quiesce_ok=TRUE;
    assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_SUCCESS && !nb.token.Active &&
      !nb.controller_ops && (nb.clock[3][0xe00/4]&4) && (nb.clock[4][0x14/4]&(1u<<26)));
  }
}
static void native_phase_setup(unsigned phase) {
  native_boot();DALI_USB_I2C_INFO info;assert(native_open(&info)==EFI_SUCCESS);native_probe();
  if(phase==3) {
    nb.hw.fail_slave_write=true;assert(nb.bus.write8(nb.bus.context,2,0xdf)==USB_IO);nb.hw.fail_slave_write=false;
  }
  if(phase==4) assert(dali_nxp_repeater_destroy(&nb.hw.nxp)==USB_OK);
}
static int native_phase(unsigned phase) {
  UINT8 value=0xa5;
  if(phase==0) {
    int r=nb.bus.read8(nb.bus.context,0x15,&value);
    assert(value==(r==USB_OK?0x32:0xa5));return r;
  }
  if(phase==1) return nb.bus.write8(nb.bus.context,2,0xdf);
  if(phase==2) return nb.bus.enable(nb.bus.context,false);
  if(phase==3) return nb.bus.recover(nb.bus.context);
  return UsbResult(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token));
}
static void native_runtime_ebs(void) {
  for(unsigned phase=0;phase<5;phase++) {
    native_phase_setup(phase);UINTN base=nb.ops;assert(native_phase(phase)==USB_OK);UINTN total=nb.ops-base;
    for(UINTN at=1;at<=total;at++) for(unsigned applied=0;applied<2;applied++) {
      native_phase_setup(phase);nb.ebs_at=nb.ops+at;nb.applied=(BOOLEAN)applied;
      assert(native_phase(phase)==USB_UNSUPPORTED && nb.ops==nb.ebs_at && mI2cBus.Closed && nb.token.Active &&
        mI2cOwner==&nb.token && !mVusb.Busy);
      UINTN operations=nb.ops;UINT8 out=0xa5;DALI_USB_I2C_INFO info,old;memset(&info,0xa5,sizeof info);old=info;
      assert(nb.bus.read8(nb.bus.context,0x15,&out)==USB_UNSUPPORTED && out==0xa5);
      assert(nb.bus.write8(nb.bus.context,2,0xef)==USB_UNSUPPORTED && nb.bus.recover(nb.bus.context)==USB_UNSUPPORTED);
      assert(mProtocol.GetI2cBusStatus(&mProtocol,&nb.token,&info)==EFI_UNSUPPORTED && !memcmp(&info,&old,sizeof info));
      assert(mProtocol.ReleaseI2cBus(&mProtocol,&nb.token)==EFI_UNSUPPORTED && nb.ops==operations && nb.token.Active);
    }
    printf("PASS: native I2C phase%u EBS before/applied effects at %llu MMIO positions\n",phase,(unsigned long long)total);
  }
  native_phase_setup(0);nb.delay_ebs=TRUE;nb.bus.delay_us(nb.bus.context,4000);
  UINT8 out=0xa5;assert(nb.bus.read8(nb.bus.context,0x15,&out)==USB_UNSUPPORTED && out==0xa5 && mI2cBus.Closed);
}
static void native_bus_tests(void) {
  assert(I2cLegacyModelMain()==0);native_normal();native_runtime_faults();native_startup_faults();native_partial_clocks();native_runtime_ebs();
  puts("PASS: installed native FIFO I2C6/GPIO72/shared gates and actual NXP probe/modes/exit, real ABI/MMIO adapter with register/policy/EBS models; hardware_qualified=false");
}
