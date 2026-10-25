#include <Library/DaliUsbPlatformLib.h>
#include <libfdt.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define P "/soc/usb-phy0@16730000"
#define U P "/usb2-phy0@16730000"
#define E "/efuse@13260000"
#define C1 E "/u2-phy-data1@1d4"
#define C2 E "/u2-phy-data2@1d8"
#define USB "/soc/usb0@16701000"
static _Alignas(8) unsigned char tree[32768];
static int node(const char *path) { int n=fdt_path_offset(tree,path); assert(n>=0); return n; }
static void add(const char *parent, const char *name) { assert(fdt_add_subnode(tree,node(parent),name)>=0); }
static void text(const char *path,const char *name,const char *value)
{ size_t length=strlen(value)+1; assert(length<=INT32_MAX); assert(fdt_setprop(tree,node(path),name,value,(int)length)==0); }
static void cells(const char *path,const char *name,const uint32_t *values,size_t count)
{
    fdt32_t encoded[8]; assert(count<=8);
    for (size_t i=0;i<count;++i) encoded[i]=cpu_to_fdt32(values[i]);
    assert(fdt_setprop(tree,node(path),name,encoded,(int)(count*4))==0);
}
static void cell(const char *path,const char *name,uint32_t value) { cells(path,name,&value,1); }
static void empty(const char *path,const char *name) { assert(fdt_setprop(tree,node(path),name,NULL,0)==0); }
static void put(unsigned char *p,uint32_t value) { for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(value>>(8*i)); }
static void fixture(void)
{
    assert(fdt_create_empty_tree(tree,sizeof tree)==0);
    text("/","compatible","mediatek,MT6991"); cell("/","#address-cells",2); cell("/","#size-cells",2);
    add("/","chosen"); add("/","soc"); cell("/soc","#address-cells",2); cell("/soc","#size-cells",2); empty("/soc","ranges");
    add("/","efuse@13260000"); text(E,"compatible","mediatek,devinfo"); cell(E,"#address-cells",1); cell(E,"#size-cells",1);
    add(E,"u2-phy-data1@1d4"); add(E,"u2-phy-data2@1d8");
    const uint32_t one[]={0x1d4,4},two[]={0x1d8,4};
    cells(C1,"reg",one,2); cells(C2,"reg",two,2); cell(C1,"phandle",11); cell(C2,"phandle",12);
    add("/soc","usb-phy0@16730000"); text(P,"compatible","mediatek,xsphy");
    cell(P,"#address-cells",2); cell(P,"#size-cells",2); empty(P,"ranges");
    const uint32_t reg[]={0,0x16730000,0,0x400}; cells(P,"reg",reg,4);
    add(P,"usb2-phy0@16730000"); cells(U,"reg",reg,4); cell(U,"#phy-cells",1); cell(U,"phandle",20);
    empty(U,"mediatek,u2-sw-efuse");
    const uint32_t refs[]={11,12,12},masks[]={0xf00000,0x7c00,0xf8000};
    cells(U,"nvmem-cells",refs,3); cells(U,"nvmem-cell-masks",masks,3);
    const char names[]="hsrx_term_cal\0hstx_impn\0hstx_impp";
    assert(fdt_setprop(tree,node(U),"nvmem-cell-names",names,sizeof names)==0);
    add("/soc","usb0@16701000"); text(USB,"compatible","mediatek,mt6991-mtu3");
    const uint32_t phys[]={20,3,21,4}; cells(USB,"phys",phys,4);
    unsigned char chip[20]={0}, dev[1284]={0}; put(chip,24); put(chip+4,0x1357); put(chip+16,1);
    put(dev,320); put(dev+4+0x1d4,0x12345678); put(dev+4+0x1d8,0xabcdef12);
    assert(fdt_setprop(tree,node("/chosen"),"atag,chipid",chip,sizeof chip)==0);
    assert(fdt_setprop(tree,node("/chosen"),"atag,devinfo",dev,sizeof dev)==0);
}
static void fail(void)
{
    struct dali_xsphy_calibration out,before;
    memset(&out,0xa5,sizeof out); memcpy(&before,&out,sizeof out);
    assert(DaliUsbReadCalibration(tree,sizeof tree,&out)!=EFI_SUCCESS && !memcmp(&out,&before,sizeof out));
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
