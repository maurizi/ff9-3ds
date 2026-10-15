#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "psx_mem.h"
unsigned char psx_ram[PSX_RAM_SIZE];
static unsigned char payload[65536],header[16];
const char *plat_data_dir(void) {return "build/tests/kernel_fixture";}
void port_log(const char *format,...) {(void)format;}
void port_fatal(const char *format,...) {(void)format;exit(2);}
static void put(unsigned char *p,unsigned v) {
    for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i));
}
static void write_image(int kind) {
    unsigned char h[16];memcpy(h,header,16);
    if(kind==1) h[0]='X';
    if(kind==2) put(h+4,65537);
    if(kind==3) h[8]^=1;
    if(kind==6) put(h+12,0);
    FILE *file=fopen("build/tests/kernel_fixture/psx_low_ram.bin","wb");assert(file);
    assert(fwrite(h,1,16,file)==16);
    unsigned size=kind==4?65535:65536;
    assert(fwrite(payload,1,size,file)==size);
    if(kind==5) fputc(0,file);
    assert(fclose(file)==0);
}
int main(void) {
    mkdir(plat_data_dir(),0700);
    remove("build/tests/kernel_fixture/psx_low_ram.bin");
    psx_ram[65536]=0xa5;psx_load_low_ram();assert(psx_ram[65536]==0xa5);
    unsigned checksum=2166136261u;
    for(unsigned i=0;i<65536;i++) {
        payload[i]=(unsigned char)(i*37+i/17);
        checksum=(checksum^payload[i])*16777619u;
    }
    memcpy(header,"PXR1",4);put(header+4,65536);put(header+8,checksum);put(header+12,0x80012000);
    write_image(0);psx_load_low_ram();
    assert(!memcmp(psx_ram,payload,65536) && psx_ram[65536]==0xa5);
    for(int kind=1;kind<=6;kind++) {
        write_image(kind);
        pid_t child=fork();assert(child>=0);
        if(!child) {psx_load_low_ram();exit(0);}
        int status;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==2);
    }
    remove("build/tests/kernel_fixture/psx_low_ram.bin");
    puts("BIOS RAM: exact bounded import, checksum, header/size/entry/truncation/trailing-data rejection pass");
}
