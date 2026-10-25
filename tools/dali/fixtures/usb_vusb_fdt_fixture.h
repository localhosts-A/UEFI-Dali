// Include after usb_clock_fdt_test.c; synthetic public test data only.
#pragma once
#define SPMI "/spmi@1c01a000"
#define PMIC5 SPMI "/second_pmic@5"
#define RAIL PMIC5 "/regulators/vusb"
#define DEBUG5 PMIC5 "/mtk-spmi-pmic-debug"
static void vusb_fixture(void) {
  clocks_fixture(); add("/","spmi@1c01a000"); text(SPMI,"compatible","mediatek,mt6991-spmi");
  cell(SPMI,"#address-cells",2); cell(SPMI,"#size-cells",0); cell(SPMI,"spmi-dev-mask",0x85c0);
  const uint32_t regs[]={0,0x1c01a000,0,0x8ff, 0,0x1c01c000,0,0x100,
    0,0x1c018000,0,0x8ff, 0,0x1c01c800,0,0x100};
  fdt32_t encoded[16]; for (unsigned i=0;i<16;i++) { encoded[i]=cpu_to_fdt32(regs[i]); }
  assert(fdt_setprop(tree,node(SPMI),"reg",encoded,sizeof encoded)==0);
  list(SPMI,"reg-names","pmif\0spmimst\0pmif-p\0spmimst-p",30);
  add(SPMI,"second_pmic@5"); text(PMIC5,"compatible","mediatek,mt6373"); cells(PMIC5,"reg",(uint32_t[]){5,0},2);
  add(SPMI,"mt6379@e"); text(SPMI "/mt6379@e","compatible","mediatek,mt6379"); cells(SPMI "/mt6379@e","reg",(uint32_t[]){14,0},2);
  add(PMIC5,"mtk-spmi-pmic-debug"); text(DEBUG5,"compatible","mediatek,spmi-pmic-debug");
  cells(DEBUG5,"cid-addr",(uint32_t[]){0x73,9,8},3);
  add(PMIC5,"regulators"); add(PMIC5 "/regulators","vusb");
  text(RAIL,"regulator-compatible","VUSB"); text(RAIL,"regulator-name","mt6373_vusb");
  cell(RAIL,"regulator-min-microvolt",1200000); cell(RAIL,"regulator-max-microvolt",3500000);
  cell(RAIL,"regulator-enable-ramp-delay",720); cells(RAIL,"regulator-allowed-modes",(uint32_t[]){0,2},2);
  empty(RAIL,"regulator-always-on"); cell(RAIL,"phandle",120); cell(USB,"vusb33-supply",120);
}
