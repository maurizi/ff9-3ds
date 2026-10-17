/* Synthetic disc: verify asynchronous delivery and reentrant command ordering. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "psx_mem.h"
unsigned char psx_ram[PSX_RAM_SIZE];
unsigned char D_8006825C, D_80068268[4];
int D_8006826C;
typedef struct {unsigned char minute,second,sector,track;} Loc;
extern Loc *CdIntToPos(int,Loc *);
extern int CdInit(void), CdControlB(unsigned char,unsigned char *,unsigned char *);
extern int CdControlF(unsigned char,unsigned char *), CD_getsector(void *,int);
extern void psx_cd_pump(void);
extern void (*CdReadyCallback(void (*)(unsigned char,unsigned char *)))(unsigned char,unsigned char *);
const char *plat_data_dir(void) {return "build/tests/cd_fixture";}
void port_fatal(const char *format,...) {
    va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);abort();
}
void psx_overlay_patch(unsigned a,unsigned b,unsigned c) {(void)a;(void)b;(void)c;abort();}
static unsigned delivered;
static unsigned char output[4][2048];
static void ready(unsigned char intr,unsigned char *result) {
    (void)result;
    assert(intr==1 && delivered<4);
    assert(CD_getsector(output[delivered],512)==0);
    delivered++;
    /* Recursive pumps must leave this sector's callback alone. */
    psx_cd_pump();
    if(delivered==2) {
        Loc next;CdIntToPos(40,&next);
        CdControlF(9,NULL);
        CdControlF(2,(unsigned char *)&next);
        CdControlF(6,NULL);
    } else if(delivered==4) CdControlF(9,NULL);
}
int main(void) {
    mkdir("build/tests/cd_fixture",0700);mkdir("build/tests/cd_fixture/raw",0700);
    FILE *file=fopen("build/tests/cd_fixture/raw/FF9 CD1.img","wb");assert(file);
    unsigned char sector[2352];
    for(unsigned i=0;i<64;i++) {
        memset(sector,0,sizeof sector);memset(sector+24,i,2048);
        assert(fwrite(sector,1,sizeof sector,file)==sizeof sector);
    }
    assert(fclose(file)==0 && CdInit());
    CdReadyCallback(ready);
    Loc start;CdIntToPos(32,&start);
    CdControlB(2,(unsigned char *)&start,NULL);assert(delivered==0);
    CdControlB(6,NULL,NULL);assert(delivered==1);
    psx_cd_pump();assert(delivered==2);
    psx_cd_pump();assert(delivered==3);
    psx_cd_pump();assert(delivered==4);
    psx_cd_pump();assert(delivered==4);
    const unsigned want[]={32,33,40,41};
    for(unsigned i=0;i<4;i++) for(unsigned j=0;j<2048;j++) assert(output[i][j]==want[i]);
    puts("CD sector events: bounded delivery, reentrant callbacks, pause/seek/read order pass");
}
