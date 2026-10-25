#include "usb_fdt_fixture.h"
#define HOST USB "/xhci0@16700000"
#define I2C "/soc/i2c@13170000"
#define REF "/clocks/clk26m"
static const char *paths[]={"/syscon@1c016000","/syscon@10000000","/syscon@10000800","/syscon@131b0000","/syscon@16640000"};
static const uint32_t bases[]={0x1c016000,0x10000000,0x10000800,0x131b0000,0x16640000};
static const uint32_t sizes[]={0x1000,0x800,0x1000,0x1000,0x1000};
static void list(const char *path,const char *name,const char *bytes,size_t length)
{ assert(length<=INT32_MAX && fdt_setprop(tree,node(path),name,bytes,(int)length)==0); }
static void clocks_fixture(void)
{
    fixture();
    const char *compat[]={"mediatek,mt6991-vlp_cksys","mediatek,mt6991-cksys","mediatek,mt6991-apmixedsys",
        "mediatek,mt6991-imp_iic_wrap_w","mediatek,mt6991-pericfg_ao"};
    for (unsigned n=0;n<5;++n) {
        add("/",paths[n]+1); char textlist[100]; size_t length=strlen(compat[n])+1;
        memcpy(textlist,compat[n],length); memcpy(textlist+length,"syscon",7);
        list(paths[n],"compatible",textlist,length+7);
        cells(paths[n],"reg",(uint32_t[]){0,bases[n],0,sizes[n]},4);
        cell(paths[n],"#clock-cells",1); cell(paths[n],"phandle",100+n);
    }
    add("/","clocks"); add("/clocks","clk26m"); text(REF,"compatible","fixed-clock");
    cell(REF,"#clock-cells",0); cell(REF,"clock-frequency",26000000); cell(REF,"phandle",110);
    cells(USB,"reg",(uint32_t[]){0,0x16701000,0,0x2e00,0,0x16703e00,0,0x100},8);
    list(USB,"reg-names","mac\0ippc",9);
    cell(USB,"#address-cells",2); cell(USB,"#size-cells",2); empty(USB,"ranges");
    text(USB,"maximum-speed","high-speed"); text(USB,"dr_mode","otg");
    empty(USB,"usb-role-switch"); empty(USB,"mediatek,clk-mgr");
    list(USB,"clock-names","sys_ck\0host_ck\0frmcnt_ck",25);
    cells(USB,"clocks",(uint32_t[]){100,34,100,35,101,36},6);
    text(U,"clock-names","ref"); cell(U,"clocks",110);
    add(USB,"xhci0@16700000"); text(HOST,"compatible","mediatek,mtk-xhci");
    cells(HOST,"reg",(uint32_t[]){0,0x16700000,0,0x1000},4); text(HOST,"reg-names","mac");
    text(HOST,"clock-names","sys_ck"); cell(HOST,"clocks",110);
    add("/soc","i2c@13170000"); text(I2C,"compatible","mediatek,mt6991-i2c");
    cells(I2C,"reg",(uint32_t[]){0,0x13170000,0,0x20000,0,0x16410000,0,0x10000},8);
    list(I2C,"clock-names","main\0dma",9); cells(I2C,"clocks",(uint32_t[]){103,5,104,42},4);
    cell(I2C,"clock-frequency",400000); cell(I2C,"clock-div",1);
}
static void fail(void)
{
    DALI_USB_CLOCK_RESOURCES out,before; memset(&out,0xa5,sizeof out); memcpy(&before,&out,sizeof out);
    assert(DaliUsbReadClockResources(tree,sizeof tree,&out)!=EFI_SUCCESS && !memcmp(&out,&before,sizeof out));
    DALI_USB_XHCI_RESOURCES xhci,xhci_before; memset(&xhci,0xa5,sizeof xhci); memcpy(&xhci_before,&xhci,sizeof xhci);
    assert(DaliUsbReadXhciResources(tree,sizeof tree,&xhci)!=EFI_SUCCESS && !memcmp(&xhci,&xhci_before,sizeof xhci));
}
static void expect_status(const void *fdt,size_t capacity,EFI_STATUS expected)
{
    DALI_USB_CLOCK_RESOURCES out,before; memset(&out,0xa5,sizeof out); memcpy(&before,&out,sizeof out);
    assert(DaliUsbReadClockResources(fdt,capacity,&out)==expected && !memcmp(&out,&before,sizeof out));
    DALI_USB_XHCI_RESOURCES xhci,xhci_before; memset(&xhci,0xa5,sizeof xhci); memcpy(&xhci_before,&xhci,sizeof xhci);
    assert(DaliUsbReadXhciResources(fdt,capacity,&xhci)==expected && !memcmp(&xhci,&xhci_before,sizeof xhci));
}
static void mutations(void)
{
    const char *disabled[]={"/","/soc",USB,HOST,P,U,I2C,"/clocks",REF};
    for (size_t n=0;n<sizeof disabled/sizeof disabled[0];++n) { clocks_fixture(); text(disabled[n],"status","disabled"); fail(); }
    for (unsigned n=0;n<5;++n) {
        clocks_fixture(); text(paths[n],"status","disabled"); fail();
        clocks_fixture(); cells(paths[n],"reg",(uint32_t[]){0,bases[n]+4,0,sizes[n]},4); fail();
        clocks_fixture(); cells(paths[n],"reg",(uint32_t[]){0,bases[n],0,sizes[n]-4},4); fail();
        clocks_fixture(); cells(paths[n],"reg",(uint32_t[]){1,bases[n],0,sizes[n]},4); fail();
        clocks_fixture(); cell(paths[n],"#clock-cells",0); fail();
        clocks_fixture(); text(paths[n],"compatible","syscon"); fail();
        clocks_fixture(); text(paths[n],"compatible","mediatek,wrong"); fail();
        clocks_fixture(); assert(fdt_set_name(tree,node(paths[n]),"moved-provider")==0); fail();
    }
    for (unsigned n=0;n<3;++n) {
        uint32_t refs[]={100,34,100,35,101,36};
        clocks_fixture(); refs[2*n+1]++; cells(USB,"clocks",refs,6); fail();
        clocks_fixture(); refs[2*n+1]--; refs[2*n]=103; cells(USB,"clocks",refs,6); fail();
    }
    for (unsigned n=0;n<2;++n) {
        uint32_t refs[]={103,5,104,42};
        clocks_fixture(); refs[2*n+1]++; cells(I2C,"clocks",refs,4); fail();
        clocks_fixture(); refs[2*n+1]--; refs[2*n]=100; cells(I2C,"clocks",refs,4); fail();
    }
    clocks_fixture(); cell(REF,"#clock-cells",1); fail();
    clocks_fixture(); cell(REF,"clock-frequency",13000000); fail();
    clocks_fixture(); cell(I2C,"clock-frequency",100000); fail();
    clocks_fixture(); cell(I2C,"clock-div",2); fail();
    clocks_fixture(); assert(fdt_delprop(tree,node(I2C),"clock-div")==0); fail();
    clocks_fixture(); text(REF,"compatible","mediatek,clock"); fail();
    clocks_fixture(); cell(U,"clocks",100); fail();
    clocks_fixture(); cell(HOST,"clocks",100); fail();
    clocks_fixture(); cells(HOST,"clocks",(uint32_t[]){110,0},2); fail();
    clocks_fixture(); cells(USB,"clocks",(uint32_t[]){100,34,100,35,101},5); fail();
    clocks_fixture(); list(USB,"clock-names","host_ck\0sys_ck\0frmcnt_ck",25); fail();
    clocks_fixture(); list(USB,"clock-names","sys_ck\0sys_ck\0frmcnt_ck",24); fail();
    clocks_fixture(); text(I2C,"clock-names","main"); fail();
    clocks_fixture(); text(HOST,"clock-names","host_ck"); fail();
    clocks_fixture(); text(U,"clock-names","other"); fail();
    clocks_fixture(); text(USB,"maximum-speed","super-speed"); fail();
    clocks_fixture(); text(USB,"dr_mode","host"); fail();
    clocks_fixture(); assert(fdt_delprop(tree,node(USB),"usb-role-switch")==0); fail();
    clocks_fixture(); cell(USB,"mediatek,clk-mgr",1); fail();
    clocks_fixture(); cell(USB,"#address-cells",1); fail();
    clocks_fixture(); cell(USB,"#size-cells",1); fail();
    clocks_fixture(); assert(fdt_delprop(tree,node(USB),"ranges")==0); fail();
    clocks_fixture(); cell(USB,"ranges",0); fail();
    clocks_fixture(); text(I2C,"compatible","mediatek,i2c"); fail();
    clocks_fixture(); text(HOST,"compatible","mediatek,other-xhci"); fail();
    clocks_fixture(); cells(I2C,"reg",(uint32_t[]){0,0x13170000,0,0x20000},4); fail();
    clocks_fixture(); cells(HOST,"reg",(uint32_t[]){0,0x16701000,0,0x1000},4); fail();
    clocks_fixture(); text(USB,"reg-names","ippc"); fail();
    clocks_fixture(); text(HOST,"reg-names","ippc"); fail();
    clocks_fixture(); add("/","duplicate"); cell("/duplicate","phandle",100); fail();
    clocks_fixture(); cell(paths[1],"linux,phandle",105); fail();
    clocks_fixture(); add("/clocks","other26m"); text("/clocks/other26m","compatible","fixed-clock");
    cell("/clocks/other26m","#clock-cells",0); cell("/clocks/other26m","clock-frequency",26000000);
    cell("/clocks/other26m","phandle",111); cell(U,"clocks",111); fail(); /* Same frequency does not prove graph identity. */
    clocks_fixture(); assert(fdt_delprop(tree,node("/chosen"),"atag,chipid")==0); fail();
    clocks_fixture(); assert(fdt_pack(tree)==0); fdt_set_size_dt_struct(tree,fdt_size_dt_struct(tree)-4);
    assert(fdt_check_header(tree)==0); fail();
    clocks_fixture(); DALI_USB_CLOCK_RESOURCES out,before; memset(&out,0xa5,sizeof out); memcpy(&before,&out,sizeof out);
    assert(DaliUsbReadClockResources(tree,sizeof tree-1,&out)!=EFI_SUCCESS && !memcmp(&out,&before,sizeof out));
    assert(DaliUsbReadClockResources(NULL,sizeof tree,&out)==EFI_INVALID_PARAMETER && !memcmp(&out,&before,sizeof out));
    assert(DaliUsbReadClockResources(tree+1,sizeof tree-1,&out)==EFI_INVALID_PARAMETER && !memcmp(&out,&before,sizeof out));
    assert(DaliUsbReadClockResources(tree,sizeof tree,NULL)==EFI_INVALID_PARAMETER);
    assert(DaliUsbReadXhciResources(tree,sizeof tree,NULL)==EFI_INVALID_PARAMETER);
    clocks_fixture(); text(paths[0],"status","disabled"); expect_status(tree,sizeof tree,EFI_UNSUPPORTED);
    clocks_fixture(); assert(fdt_delprop(tree,node("/chosen"),"atag,chipid")==0);
    expect_status(tree,sizeof tree,EFI_UNSUPPORTED);
    clocks_fixture(); fdt_set_magic(tree,0); expect_status(tree,sizeof tree,EFI_COMPROMISED_DATA);
    clocks_fixture(); expect_status(tree,39,EFI_INVALID_PARAMETER); expect_status(tree,40,EFI_COMPROMISED_DATA);
    size_t maximum=16u*1024*1024;
    void *large=malloc(maximum+1); assert(large); memcpy(large,tree,sizeof tree);
    expect_status(large,maximum+1,EFI_INVALID_PARAMETER);
    assert(DaliUsbReadClockResources(large,maximum,&out)==EFI_SUCCESS && out.ReferenceHz==26000000);
    DALI_USB_XHCI_RESOURCES xhci; assert(DaliUsbReadXhciResources(large,maximum,&xhci)==EFI_SUCCESS);
    assert(xhci.Registers.Base==0x16700000 && xhci.Registers.Size==0x1000); free(large);
}
static void check(const DALI_USB_CLOCK_RESOURCES *out)
{
    assert(out->ReferenceHz==26000000 && out->Calibration.qualified && out->Calibration.sw_ver==1);
    for (unsigned n=0;n<5;++n) assert(out->Bank[n].Base==bases[n] && out->Bank[n].Size==sizes[n]);
}
static void check_xhci(const DALI_USB_XHCI_RESOURCES *out)
{
    assert(out->Registers.Base==0x16700000 && out->Registers.Size==0x1000);
}
int main(int argc,char **argv)
{
    clocks_fixture(); DALI_USB_CLOCK_RESOURCES out={0};
    assert(DaliUsbReadClockResources(tree,sizeof tree,&out)==EFI_SUCCESS); check(&out);
    DALI_USB_XHCI_RESOURCES xhci={0}; assert(DaliUsbReadXhciResources(tree,sizeof tree,&xhci)==EFI_SUCCESS); check_xhci(&xhci);
    for (unsigned n=0;n<5;++n) cell(paths[n],"linux,phandle",100+n);
    assert(DaliUsbReadClockResources(tree,sizeof tree,&out)==EFI_SUCCESS); check(&out);
    assert(DaliUsbReadXhciResources(tree,sizeof tree,&xhci)==EFI_SUCCESS); check_xhci(&xhci);
    assert(fdt_set_name(tree,node("/chosen"),"chosen@0")==0);
    assert(DaliUsbReadClockResources(tree,sizeof tree,&out)==EFI_SUCCESS); check(&out);
    assert(DaliUsbReadXhciResources(tree,sizeof tree,&xhci)==EFI_SUCCESS); check_xhci(&xhci);
    mutations();
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb"); assert(f && fseek(f,0,SEEK_END)==0); long length=ftell(f); assert(length>40 && length<=16*1024*1024);
        rewind(f); void *saved=malloc((size_t)length); assert(saved && fread(saved,1,(size_t)length,f)==(size_t)length); fclose(f);
        assert(DaliUsbReadClockResources(saved,(size_t)length,&out)==EFI_SUCCESS); check(&out);
        assert(DaliUsbReadXhciResources(saved,(size_t)length,&xhci)==EFI_SUCCESS); check_xhci(&xhci); free(saved);
        puts("PASS: saved Dali FDT clock resource acquisition; no raw ATAG data printed");
    }
    puts("PASS: production UEFI clock resource graph and calibration, pinned real libfdt; no hardware access");
    return 0;
}
