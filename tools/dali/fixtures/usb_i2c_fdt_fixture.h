// Include after usb_clock_fdt_test.c; synthetic public test data only.
#pragma once
#define PINCTRL_PATH "/soc/pinctrl@1002d000"
#define NXP_PATH I2C "/nxp-eusb2-repeater@4f"
#define RESET_ON PINCTRL_PATH "/rptr-enable"
#define RESET_OFF PINCTRL_PATH "/rptr-disable"
static const uint32_t pin_bases[]={0x1002d000,0x12000000,0x12020000,0x12040000,0x12060000,0x12820000,
 0x12840000,0x12860000,0x13000000,0x13020000,0x13040000,0x130f0000,0x13110000,0x13800000,0x13820000,0x13860000};
static void pin_regions(unsigned changed,int element,uint32_t value) {
  fdt32_t regs[64];
  for(unsigned i=0;i<16;i++) {
    regs[4*i]=cpu_to_fdt32(0); regs[4*i+1]=cpu_to_fdt32(pin_bases[i]);
    regs[4*i+2]=cpu_to_fdt32(0); regs[4*i+3]=cpu_to_fdt32(0x1000);
  }
  if(changed<16) regs[changed*4+(unsigned)element]=cpu_to_fdt32(value);
  assert(fdt_setprop(tree,node(PINCTRL_PATH),"reg",regs,sizeof regs)==0);
}
static void add_i2c_fixture(void) {
  add("/soc","pinctrl@1002d000"); text(PINCTRL_PATH,"compatible","mediatek,mt6991-pinctrl");
  pin_regions(16,0,0);
  const char names[]="gpio\0iocfg_rt\0iocfg_rm1\0iocfg_rm2\0iocfg_rb\0iocfg_bm1\0iocfg_bm2\0iocfg_bm3\0iocfg_lt\0iocfg_lm1\0iocfg_lm2\0iocfg_lb1\0iocfg_lb2\0iocfg_tm1\0iocfg_tm2\0iocfg_tm3";
  list(PINCTRL_PATH,"reg-names",names,sizeof names);
  empty(PINCTRL_PATH,"gpio-controller"); cell(PINCTRL_PATH,"#gpio-cells",2); cell(PINCTRL_PATH,"phandle",130);
  cells(PINCTRL_PATH,"gpio-ranges",(uint32_t[]){130,0,0,271},4);
  cell(I2C,"#address-cells",1); cell(I2C,"#size-cells",0);
  cell(I2C,"scl-gpio-id",104); cell(I2C,"sda-gpio-id",105);
  add(I2C,"nxp-eusb2-repeater@4f"); text(NXP_PATH,"compatible","mtk,nxp-eusb2-repeater");
  cell(NXP_PATH,"reg",0x4f); cell(NXP_PATH,"#phy-cells",0); cell(NXP_PATH,"phandle",131);
  list(NXP_PATH,"pinctrl-names","enable\0disable",15);
  add(PINCTRL_PATH,"rptr-enable"); cell(RESET_ON,"phandle",132);
  add(RESET_ON,"pins-cmd-dat"); cell(RESET_ON "/pins-cmd-dat","pinmux",72U<<8); empty(RESET_ON "/pins-cmd-dat","output-high");
  add(PINCTRL_PATH,"rptr-disable"); cell(RESET_OFF,"phandle",133);
  add(RESET_OFF,"pins-cmd-dat"); cell(RESET_OFF "/pins-cmd-dat","pinmux",72U<<8); empty(RESET_OFF "/pins-cmd-dat","output-low");
  cell(NXP_PATH,"pinctrl-0",132); cell(NXP_PATH,"pinctrl-1",133);
}
