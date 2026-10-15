/* Guest BIOS RAM is data, never executable native code. Keep original guest
 * words intact: accidental game reads can consume kernel instructions as data. */
#include "psx_mem.h"
#include "psx_runtime.h"
#include "platform.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned word(const unsigned char *p) {
    return p[0] | (unsigned)p[1]<<8 | (unsigned)p[2]<<16 | (unsigned)p[3]<<24;
}
void psx_load_low_ram(void) {
    char path[512];
    int n=snprintf(path,sizeof path,"%s/psx_low_ram.bin",plat_data_dir());
    if(n<0 || (unsigned)n>=sizeof path) port_fatal("PSX low RAM: data path too long");
    FILE *file=fopen(path,"rb");
    if(!file) {
        if(errno!=ENOENT) port_fatal("PSX low RAM: cannot open %s",path);
        port_log("PSX low RAM: zero-initialized (no psx_low_ram.bin)\n");
        return;
    }
    unsigned char header[16];
    if(fread(header,1,sizeof header,file)!=sizeof header || memcmp(header,"PXR1",4) ||
       word(header+4)!=0x10000 || word(header+12)<0x80010000 || word(header+12)>=0x80200000)
        port_fatal("PSX low RAM: invalid header in %s",path);
    if(fread(psx_ram,1,0x10000,file)!=0x10000 || fgetc(file)!=EOF || ferror(file))
        port_fatal("PSX low RAM: expected exactly 65536 payload bytes in %s",path);
    fclose(file);
    unsigned checksum=2166136261u;
    for(unsigned i=0;i<0x10000;i++) checksum=(checksum^psx_ram[i])*16777619u;
    if(checksum!=word(header+8)) port_fatal("PSX low RAM: checksum mismatch in %s",path);
    port_log("PSX low RAM: loaded BIOS boot image\n");
}
