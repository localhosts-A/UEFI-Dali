#pragma once
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
