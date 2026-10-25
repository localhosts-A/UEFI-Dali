#define main ClockGraphMain
#include "usb_clock_fdt_test.c"
#undef main
#include "usb_vusb_fdt_fixture.h"
static void valid_vusb(const DALI_USB_VUSB_RESOURCES *out) {
  assert(out->SpmiDeviceMask==0x85c0 && out->MinMicrovolt==1200000 && out->MaxMicrovolt==3500000 && out->EnableRampMicroseconds==720);
}
static void reject_vusb(void) {
  DALI_USB_VUSB_RESOURCES out,old; memset(&out,0xa5,sizeof out); old=out;
  assert(DaliUsbReadVusbResources(tree,sizeof tree,&out)!=EFI_SUCCESS && !memcmp(&out,&old,sizeof out));
}
int main(int argc,char **argv) {
  assert(ClockGraphMain(1,argv)==0);
  vusb_fixture(); DALI_USB_VUSB_RESOURCES out;
  assert(DaliUsbReadVusbResources(tree,sizeof tree,&out)==EFI_SUCCESS); valid_vusb(&out);
  const char *disabled[]={"/","/soc",USB,SPMI,PMIC5,DEBUG5,PMIC5 "/regulators",RAIL};
  for (unsigned i=0;i<sizeof disabled/sizeof disabled[0];i++) { vusb_fixture(); text(disabled[i],"status","disabled"); reject_vusb(); }
  vusb_fixture(); cells(PMIC5,"reg",(uint32_t[]){14,0},2); reject_vusb();
  vusb_fixture(); text(PMIC5,"compatible","mediatek,mt6379"); reject_vusb();
  vusb_fixture(); cells(DEBUG5,"cid-addr",(uint32_t[]){0x79,9,8},3); reject_vusb();
  vusb_fixture(); cells(DEBUG5,"cid-addr",(uint32_t[]){0x73,8,9},3); reject_vusb();
  vusb_fixture(); text(DEBUG5,"compatible","mediatek,other"); reject_vusb();
  const char *props[]={"regulator-min-microvolt","regulator-max-microvolt","regulator-enable-ramp-delay"};
  for(unsigned i=0;i<3;i++) { vusb_fixture(); cell(RAIL,props[i],0); reject_vusb(); }
  vusb_fixture(); cells(RAIL,"regulator-allowed-modes",(uint32_t[]){0,1},2); reject_vusb();
  vusb_fixture(); cell(RAIL,"regulator-always-on",1); reject_vusb();
  vusb_fixture(); text(RAIL,"regulator-name","mt6379_otg_vbus"); reject_vusb();
  vusb_fixture(); text(RAIL,"regulator-compatible","other"); reject_vusb();
  vusb_fixture(); assert(fdt_delprop(tree,node(RAIL),"regulator-always-on")==0); reject_vusb();
  vusb_fixture(); cell(SPMI,"spmi-dev-mask",0x85e0); reject_vusb();
  vusb_fixture(); cell(SPMI,"spmi-dev-mask",0x185c0); reject_vusb();
  vusb_fixture(); cell(SPMI,"#address-cells",1); reject_vusb();
  vusb_fixture(); list(SPMI,"reg-names","pmif\0spmimst\0pmif-p\0bad",24); reject_vusb();
  vusb_fixture(); cells(SPMI,"reg",(uint32_t[]){0,0x1c01a000,0,0x900},4); reject_vusb();
  vusb_fixture(); cell(USB,"vusb33-supply",0); reject_vusb();
  vusb_fixture(); cells(USB,"vusb33-supply",(uint32_t[]){120,0},2); reject_vusb();
  vusb_fixture(); add("/","other-rail"); cell("/other-rail","phandle",120); reject_vusb();
  vusb_fixture(); assert(fdt_pack(tree)==0); fdt_set_size_dt_struct(tree,fdt_size_dt_struct(tree)-4); reject_vusb();
  if (argc==2) {
    FILE *f=fopen(argv[1],"rb"); assert(f && fseek(f,0,SEEK_END)==0); long length=ftell(f); assert(length>40);
    rewind(f); void *saved=malloc((size_t)length); assert(saved && fread(saved,1,(size_t)length,f)==(size_t)length); fclose(f);
    assert(DaliUsbReadVusbResources(saved,(UINTN)length,&out)==EFI_SUCCESS); valid_vusb(&out); free(saved);
    puts("PASS: saved Dali FDT MT6373 VUSB supply acquisition; no raw payload published");
  }
  puts("PASS: production UEFI MT6373 VUSB FDT graph; pinned real libfdt, no hardware access"); return 0;
}
