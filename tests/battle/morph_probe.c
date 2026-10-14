#include "psyq_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
extern u32 g_vtxScreenXY[],g_vtxScreenZ[];
void o0b_func_800DF15C(const void *,u32,u32);
void oracle_set(const void *);
void oracle_get(void *);
int main(void) {
    if(mmap(psx_ram,PSX_RAM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=psx_ram) abort();
    u32 args[2],state[64];
    while(fread(args,sizeof args,1,stdin)==1) {
        if(fread(psx_ram+0x180000,8192,1,stdin)!=1 || fread(g_vtxScreenXY,512,1,stdin)!=1 ||
           fread(g_vtxScreenZ,512,1,stdin)!=1 || fread(state,sizeof state,1,stdin)!=1) abort();
        *(u32 *)(psx_ram+0x6794c)=(u32)(psx_ram+0x180100);
        oracle_set(state);
        o0b_func_800DF15C(psx_ram+0x180000,args[0],args[1]);
        oracle_get(state);
        fwrite(g_vtxScreenXY,512,1,stdout);fwrite(g_vtxScreenZ,512,1,stdout);fwrite(state,sizeof state,1,stdout);
    }
}
