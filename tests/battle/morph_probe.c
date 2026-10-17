#include "psyq_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
extern u32 g_vtxScreenXY[],g_vtxScreenZ[];
void o0b_btlfx_projectMorphMesh(const void *,u32,u32);
void o0b_btlfx_projectSliceMesh(const void *,u8);
void oracle_set(const void *);
void oracle_get(void *);
int main(void) {
    if(mmap(psx_ram,PSX_RAM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=psx_ram) abort();
    u32 args[3],state[64];
    while(fread(args,sizeof args,1,stdin)==1) {
        if(fread(psx_ram+0x180000,8192,1,stdin)!=1 || fread(g_vtxScreenXY,512,1,stdin)!=1 ||
           fread(g_vtxScreenZ,512,1,stdin)!=1 || fread(state,sizeof state,1,stdin)!=1) abort();
        *(u32 *)(psx_ram+0x6794c)=(u32)(psx_ram+0x180100);
        oracle_set(state);
        if(args[0]) o0b_btlfx_projectSliceMesh(psx_ram+0x180000,(u8)args[1]);
        else o0b_btlfx_projectMorphMesh(psx_ram+0x180000,args[1],args[2]);
        oracle_get(state);
        fwrite(g_vtxScreenXY,512,1,stdout);fwrite(g_vtxScreenZ,512,1,stdout);fwrite(state,sizeof state,1,stdout);
    }
}
