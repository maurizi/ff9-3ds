/* Full-resolution RGB24 and production DS scaler output for emulator comparison.
 * stdin: 1 MiB VRAM. stdout: 320x240 RGB8, then 256x192 DS RGB555 halfwords. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "psx_display.h"
#include "display_scale.h"
static uint16_t vram[1024*512], scaled[256*192];
int main(int argc, char **argv) {
    if (argc != 3 || fread(vram,1,sizeof vram,stdin)!=sizeof vram) return 2;
    int x=atoi(argv[1]), y=atoi(argv[2]);
    for (int row=0;row<240;row++) for (int col=0;col<320;col++) {
        uint32_t rgb=psx_display_rgb24(vram,x,y+row,col);
        unsigned char bytes[3]={rgb,rgb>>8,rgb>>16};
        if (fwrite(bytes,1,3,stdout)!=3) return 3;
    }
    ds_scale_display_rgb24(scaled,vram,x,y,320,240);
    return fwrite(scaled,1,sizeof scaled,stdout)==sizeof scaled ? 0 : 3;
}
