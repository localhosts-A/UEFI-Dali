#define main ClockGraphMain
#include "usb_clock_fdt_test.c"
#undef main
#include "usb_i2c_fdt_fixture.h"
static void i2c_fixture(void) { clocks_fixture(); add_i2c_fixture(); }
static void i2c_failure(void) {
  DALI_USB_I2C_RESOURCES out,old; memset(&out,0xa5,sizeof out); old=out;
  assert(DaliUsbReadI2cResources(tree,sizeof tree,&out)!=EFI_SUCCESS && !memcmp(&out,&old,sizeof out));
}
static void i2c_check(const DALI_USB_I2C_RESOURCES *out) {
  assert(out->Gpio.Base==0x1002d000 && out->Gpio.Size==0x1000 && out->IocfgLb2.Base==0x13110000 && out->IocfgLb2.Size==0x1000);
  assert(out->Controller.Base==0x13170000 && out->Controller.Size==0x20000 && out->Dma.Base==0x16410000 && out->Dma.Size==0x10000);
  assert(out->SclPin==104 && out->SdaPin==105 && out->ResetPin==72);
}
int main(int argc,char **argv) {
  i2c_fixture(); DALI_USB_I2C_RESOURCES out;
  assert(DaliUsbReadI2cResources(tree,sizeof tree,&out)==EFI_SUCCESS); i2c_check(&out);
  const char *disabled[]={"/","/soc",I2C,PINCTRL_PATH,NXP_PATH,RESET_ON,RESET_OFF,RESET_ON "/pins-cmd-dat",RESET_OFF "/pins-cmd-dat"};
  for(unsigned n=0;n<sizeof disabled/sizeof disabled[0];n++) { i2c_fixture(); text(disabled[n],"status","disabled"); i2c_failure(); }
  for(unsigned n=0;n<16;n++) for(int element=0;element<4;element++) {
    i2c_fixture(); pin_regions(n,element,element==1?pin_bases[n]+4:element==3?0x800:1); i2c_failure();
  }
  const char *props[]={"ch-offset-i2c","ch-offset-dma","i2c-offset-ap","#address-cells","#size-cells","scl-gpio-id","sda-gpio-id"};
  for(unsigned n=0;n<sizeof props/sizeof props[0];n++) { i2c_fixture(); cell(I2C,props[n],7); i2c_failure(); }
  i2c_fixture(); cell(I2C,"ch-offset-i2c",0); cell(I2C,"ch-offset-dma",0); cell(I2C,"i2c-offset-ap",0x100);
  assert(DaliUsbReadI2cResources(tree,sizeof tree,&out)==EFI_SUCCESS);
  const char *unsupported[]={"mediatek,use-push-pull","mediatek,disable-clk-stretch","mediatek,have-pmic","mediatek,control-irq-sel","mediatek,control-rs-stop","mediatek,wake-scp-check-en"};
  for(unsigned n=0;n<sizeof unsupported/sizeof unsupported[0];n++) { i2c_fixture(); empty(I2C,unsupported[n]); i2c_failure(); }
  i2c_fixture(); text(PINCTRL_PATH,"compatible","mediatek,gpio"); i2c_failure();
  i2c_fixture(); text(PINCTRL_PATH,"reg-names","iocfg_lb2"); i2c_failure();
  i2c_fixture(); cell(PINCTRL_PATH,"#gpio-cells",1); i2c_failure();
  i2c_fixture(); assert(fdt_delprop(tree,node(PINCTRL_PATH),"gpio-controller")==0); i2c_failure();
  i2c_fixture(); cells(PINCTRL_PATH,"gpio-ranges",(uint32_t[]){130,0,0,270},4); i2c_failure();
  i2c_fixture(); cells(PINCTRL_PATH,"gpio-ranges",(uint32_t[]){131,0,0,271},4); i2c_failure();
  i2c_fixture(); text(NXP_PATH,"compatible","nxp,eusb2-repeater"); i2c_failure();
  i2c_fixture(); cell(NXP_PATH,"reg",0x4e); i2c_failure();
  i2c_fixture(); cell(NXP_PATH,"#phy-cells",1); i2c_failure();
  i2c_fixture(); list(NXP_PATH,"pinctrl-names","disable\0enable",15); i2c_failure();
  i2c_fixture(); cell(NXP_PATH,"pinctrl-0",133); i2c_failure();
  i2c_fixture(); cells(NXP_PATH,"pinctrl-1",(uint32_t[]){133,132},2); i2c_failure();
  i2c_fixture(); cell(RESET_ON "/pins-cmd-dat","pinmux",73U<<8); i2c_failure();
  i2c_fixture(); cell(RESET_OFF "/pins-cmd-dat","pinmux",(72U<<8)|1); i2c_failure();
  i2c_fixture(); empty(RESET_ON "/pins-cmd-dat","output-low"); i2c_failure();
  i2c_fixture(); cell(RESET_OFF "/pins-cmd-dat","output-low",0); i2c_failure();
  i2c_fixture(); empty(RESET_ON "/pins-cmd-dat","bias-pull-down"); i2c_failure();
  i2c_fixture(); empty(RESET_ON "/pins-cmd-dat","input-enable"); i2c_failure();
  i2c_fixture(); cell(RESET_OFF,"pinmux",73U<<8); i2c_failure();
  i2c_fixture(); text(RESET_ON,"status","okay"); text(RESET_ON "/pins-cmd-dat","status","okay");
  assert(DaliUsbReadI2cResources(tree,sizeof tree,&out)==EFI_SUCCESS);
  i2c_fixture(); add(RESET_ON,"another-group"); cell(RESET_ON "/another-group","pinmux",73U<<8); i2c_failure();
  i2c_fixture(); add(RESET_ON "/pins-cmd-dat","nested-group"); i2c_failure();
  i2c_fixture(); add("/","duplicate"); cell("/duplicate","phandle",132); i2c_failure();
  i2c_fixture(); cell(RESET_OFF,"linux,phandle",134); i2c_failure();
  i2c_fixture(); cell("/soc","ranges",1); i2c_failure();
  i2c_fixture(); fdt_set_magic(tree,0); i2c_failure();
  assert(DaliUsbReadI2cResources(NULL,0,&out)==EFI_INVALID_PARAMETER);
  assert(DaliUsbReadI2cResources(tree,sizeof tree,NULL)==EFI_INVALID_PARAMETER);
  if(argc==2) {
    FILE *f=fopen(argv[1],"rb"); assert(f && fseek(f,0,SEEK_END)==0); long length=ftell(f); assert(length>40);
    rewind(f); void *saved=malloc((size_t)length); assert(saved && fread(saved,1,(size_t)length,f)==(size_t)length); fclose(f);
    assert(DaliUsbReadI2cResources(saved,(size_t)length,&out)==EFI_SUCCESS); i2c_check(&out); free(saved);
    puts("PASS: saved Dali FDT I2C6 pin/reset graph acquisition; no raw payload published");
  }
  puts("PASS: production UEFI I2C6 pin/reset resource graph, real pinned libfdt; no power/pin/electrical qualification");
  return 0;
}
