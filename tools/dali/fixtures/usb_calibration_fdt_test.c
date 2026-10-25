#include "usb_fdt_fixture.h"
static void fail(void)
{
    struct dali_xsphy_calibration out,before;
    memset(&out,0xa5,sizeof out); memcpy(&before,&out,sizeof out);
    assert(DaliUsbReadCalibration(tree,sizeof tree,&out)!=EFI_SUCCESS && !memcmp(&out,&before,sizeof out));
}
static void expect_status(const void *fdt,size_t capacity,EFI_STATUS expected)
{
    struct dali_xsphy_calibration out,before;
    memset(&out,0xa5,sizeof out); memcpy(&before,&out,sizeof out);
    assert(DaliUsbReadCalibration(fdt,capacity,&out)==expected && !memcmp(&out,&before,sizeof out));
}
static void mutations(void)
{
    const char *disabled[]={"/","/soc",USB,P,U,E,C1,C2,"/chosen"};
    for (size_t n=0;n<sizeof disabled/sizeof disabled[0];++n) { fixture(); text(disabled[n],"status","disabled"); fail(); }
    fixture(); text("/","compatible","mediatek,other"); fail();
    fixture(); const char badcompatible[]="mediatek,MT6991\0trailing";
    assert(fdt_setprop(tree,node("/"),"compatible",badcompatible,sizeof badcompatible-1)==0); fail();
    fixture(); text(E,"compatible","mediatek,efuse"); fail();
    fixture(); cell("/soc","#address-cells",1); fail();
    fixture(); cell(P,"#size-cells",1); fail();
    fixture(); cell(U,"#phy-cells",2); fail();
    fixture(); cell(C1,"reg",0x1d4); fail();
    fixture(); const uint32_t wrongreg[]={0x1d0,4}; cells(C1,"reg",wrongreg,2); fail();
    fixture(); const uint32_t refs[]={11,12,11}; cells(U,"nvmem-cells",refs,3); fail();
    fixture(); const uint32_t masks[]={0xf00000,0x7c00,0xf8001}; cells(U,"nvmem-cell-masks",masks,3); fail();
    fixture(); text(U,"nvmem-cell-names","hsrx_term_cal"); fail();
    fixture(); assert(fdt_delprop(tree,node(U),"mediatek,u2-sw-efuse")==0); fail();
    fixture(); add("/","duplicate"); cell("/duplicate","phandle",11); fail();
    fixture(); cell(C1,"linux,phandle",12); fail();
    fixture(); cell(C1,"phandle",0); fail();
    fixture(); cell(C1,"phandle",0xffffffff); fail();
    fixture(); add("/","other-devinfo"); text("/other-devinfo","compatible","mediatek,devinfo");
    cell("/other-devinfo","#address-cells",1); cell("/other-devinfo","#size-cells",1);
    add("/other-devinfo","cell@1d8"); cells("/other-devinfo/cell@1d8","reg",(uint32_t[]){0x1d8,4},2);
    cell("/other-devinfo/cell@1d8","phandle",13); cells(U,"nvmem-cells",(uint32_t[]){11,13,13},3); fail();
    fixture(); const uint32_t phys[]={11,3,21,4}; cells(USB,"phys",phys,4); fail();
    fixture(); assert(fdt_delprop(tree,node("/chosen"),"atag,chipid")==0); fail();
    fixture(); assert(fdt_delprop(tree,node("/chosen"),"atag,devinfo")==0); fail();
    fixture(); unsigned char shorttag[19]={0}; assert(fdt_setprop(tree,node("/chosen"),"atag,chipid",shorttag,sizeof shorttag)==0); fail();
    fixture(); unsigned char baddev[4]={0xff,0xff,0xff,0xff}; assert(fdt_setprop(tree,node("/chosen"),"atag,devinfo",baddev,sizeof baddev)==0); fail();
    fixture(); fdt_set_totalsize(tree,0xffffffff); fail();
    fixture(); fdt_set_off_dt_struct(tree,0xfffffff0); fail();
    fixture(); fdt_set_size_dt_struct(tree,0xfffffff0); fail();
    fixture(); fdt_set_magic(tree,0); fail();
    fixture(); assert(fdt_pack(tree)==0); uint32_t size=fdt_size_dt_struct(tree);
    fdt_set_size_dt_struct(tree,size-4); /* Remove FDT_END but retain every needed node/property. */
    assert(fdt_check_header(tree)==0 && fdt_check_full(tree,sizeof tree)<0); fail();
    fixture(); assert(fdt_pack(tree)==0); size=fdt_size_dt_struct(tree);
    fdt32_t *end=(fdt32_t *)(tree+fdt_off_dt_struct(tree)+size-8);
    assert(fdt32_to_cpu(*end)==FDT_END_NODE); *end=cpu_to_fdt32(FDT_NOP); /* Missing root close. */
    assert(fdt_check_header(tree)==0 && fdt_check_full(tree,sizeof tree)<0); fail();
    fixture(); struct dali_xsphy_calibration out;
    for (size_t n=0;n<sizeof tree;++n) assert(DaliUsbReadCalibration(tree,n,&out)!=EFI_SUCCESS);
    assert(DaliUsbReadCalibration(tree+1,sizeof tree-1,&out)==EFI_INVALID_PARAMETER);
    assert(DaliUsbReadCalibration(NULL,sizeof tree,&out)==EFI_INVALID_PARAMETER);
    assert(DaliUsbReadCalibration(tree,sizeof tree,NULL)==EFI_INVALID_PARAMETER);
    fixture(); assert(fdt_delprop(tree,node("/chosen"),"atag,chipid")==0);
    expect_status(tree,sizeof tree,EFI_UNSUPPORTED);
    fixture(); unsigned char malformed[19]={0};
    assert(fdt_setprop(tree,node("/chosen"),"atag,chipid",malformed,sizeof malformed)==0);
    expect_status(tree,sizeof tree,EFI_COMPROMISED_DATA);
    fixture(); fdt_set_magic(tree,0); expect_status(tree,sizeof tree,EFI_COMPROMISED_DATA);
    fixture(); expect_status(tree,39,EFI_INVALID_PARAMETER);
    expect_status(tree,40,EFI_COMPROMISED_DATA);
    size_t maximum=16u*1024*1024;
    void *large=malloc(maximum+1); assert(large); memcpy(large,tree,sizeof tree);
    expect_status(large,maximum+1,EFI_INVALID_PARAMETER);
    assert(DaliUsbReadCalibration(large,maximum,&out)==EFI_SUCCESS && out.qualified); free(large);
}
int main(int argc,char **argv)
{
    fixture(); struct dali_xsphy_calibration out={0};
    assert(DaliUsbReadCalibration(tree,sizeof tree,&out)==EFI_SUCCESS && out.qualified && out.sw_ver==1 &&
        out.efuse_1d4==0x12345678 && out.efuse_1d8==0xabcdef12);
    cell(C1,"linux,phandle",11);
    assert(DaliUsbReadCalibration(tree,sizeof tree,&out)==EFI_SUCCESS);
    assert(fdt_set_name(tree,node("/chosen"),"chosen@0")==0);
    assert(DaliUsbReadCalibration(tree,sizeof tree,&out)==EFI_SUCCESS);
    mutations();
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb"); assert(f && fseek(f,0,SEEK_END)==0); long length=ftell(f); assert(length>40 && length<=16*1024*1024);
        rewind(f); void *saved=malloc((size_t)length); assert(saved && fread(saved,1,(size_t)length,f)==(size_t)length); fclose(f);
        assert(DaliUsbReadCalibration(saved,(size_t)length,&out)==EFI_SUCCESS && out.qualified && out.sw_ver==1); free(saved);
        puts("PASS: saved Dali FDT calibration acquisition; no raw ATAG data printed");
    }
    puts("PASS: production UEFI calibration graph and ATAG decoder linked with pinned real libfdt; no hardware access");
    return 0;
}
