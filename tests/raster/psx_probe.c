/* Synthetic VRAM and GP0 streams supplied by test_raster_psx.py. */
#include <stdint.h>
#include <stdio.h>
#include "gp0.h"
static uint16_t bitmap[1024*512];
uint16_t *psx_vram=bitmap;
extern const Gp0Backend psx_softgpu;
/* The full GPU driver handles VRAM fills before the raster backend. Supply
 * that setup operation here so each blended draw starts with the same data. */
static void fill(int x,int y,int w,int h,unsigned rgb) {
    uint16_t c=((rgb&255)>>3)|((((rgb>>8)&255)>>3)<<5)|((((rgb>>16)&255)>>3)<<10);
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) bitmap[((y+j)&511)*1024+((x+i)&1023)]=c;
}
int main(void) {
    uint32_t count,packet[128];
    if(fread(bitmap,sizeof bitmap,1,stdin)!=1) return 2;
    Gp0Backend backend=psx_softgpu;backend.fill=fill;gp0_backend=&backend;
    while(fread(&count,sizeof count,1,stdin)==1) {
        if(count>128 || fread(packet,4,count,stdin)!=count) return 2;
        gp0_run(packet,count);
        for(int y=4;y<8;y++) if(fwrite(bitmap+y*1024+4,2,4,stdout)!=4) return 2;
    }
    return ferror(stdin) || ferror(stdout);
}
