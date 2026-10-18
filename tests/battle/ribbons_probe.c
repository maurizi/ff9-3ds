#include "psyq_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../../port/psx/gte.c"
u8 psx_scratchpad[0x400];
void port_fatal(const char *fmt,...) { (void)fmt;abort(); }
void o0b_btlfx_emitTrailRibbonScreen(s32,void *,u8 *,u32 *);
void o0b_btlfx_emitTrailRibbon3D(s32,void *,u8 *,u32 *);
void o0b_btlfx_emitTrailRibbonFlat(s32,const void *,u8 *,u32 *);
void o0b_func_800DFF5C(u8 *,s32,const s16 *,s16 *);
void o0b_func_800DFF64(u8 *,s32,const s16 *,s16 *);
void o0b_func_800E0014(u8 *,s32,const s16 *,s16 *);
int main(void) {
    if(mmap(psx_ram,PSX_RAM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=psx_ram) abort();
    if(mmap((void *)0x02400000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=(void *)0x02400000) abort();
    if(fread(psx_ram+0xcfa00,512,1,stdin)!=1) abort();
    u8 *data=psx_ram+0x180000,*out=data+0x1000;u32 *ot=(u32 *)(data+0x8000),count,kind;
    while(fread(&kind,4,1,stdin)==1) {
        if(fread(&count,4,1,stdin)!=1) abort();
        if(fread(data,512,1,stdin)!=1 || fread(out,1024,1,stdin)!=1 || fread(ot,16384,1,stdin)!=1 || fread(&g,sizeof g,1,stdin)!=1) abort();
        if(kind==5) {
            const unsigned bias_offsets[]={0,128,12,0};
            unsigned b=data[511]&3,o=data[510]&3;
            o0b_func_800E0014(data,(s32)count,b ? (s16 *)(data+bias_offsets[b]) : NULL,
                             (s16 *)(o ? data+(o-1)*8 : out));
        }
        else if(kind==3 || kind==4) {
            if(*(u32 *)(data+4)==0x02400080) __builtin_memcpy((void *)0x02400080,data+128,128);
            if(kind==3) o0b_btlfx_emitTrailRibbon3D((s32)count,data,out,ot);
            else o0b_btlfx_emitTrailRibbonFlat((s32)count,data,out,ot);
        }
        else if(!kind) o0b_btlfx_emitTrailRibbonScreen((s32)count,data,out,ot);
        else if(kind==1) o0b_func_800DFF5C(data,(s32)count,(s16 *)(data+128),(s16 *)out);
        else o0b_func_800DFF64(data,(s32)count,(s16 *)(data+128),(s16 *)out);
        fwrite(data,512,1,stdout);fwrite(out,1024,1,stdout);fwrite(ot,16384,1,stdout);fwrite(&g,sizeof g,1,stdout);
    }
}
