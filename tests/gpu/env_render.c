/* Exercise clear packets through the real decoder and CPU rasteriser. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#undef NULL
#include "psyq_port.h"
#include "gp0.h"
#include "psx_raster.h"

unsigned char psx_ram[PSX_RAM_SIZE];
extern u16 *psx_vram;
static u16 bitmap[1024*512];
void port_log(const char *format, ...) { (void)format; }
void port_fatal(const char *format, ...) { (void)format; abort(); }

static void check_clear(int x, int y, int w, int h) {
    for (int j=0;j<512;j++) for (int i=0;i<1024;i++)
        assert(bitmap[j*1024+i] == (i>=x && i<x+w && j>=y && j<y+h ? 0x421 : 0x1234));
}
int main(void) {
    psx_vram=bitmap;gp0_backend=&psx_softgpu;
    /* Odd pixel counts are still whole GPU words. Check both VRAM wrapping
     * and zero padding, without overwriting the caller's adjacent words. */
    RECT transfer={1023,511,3,3};
    u32 pixels[5]={0x00020001,0x00040003,0x00060005,0x00080007,0xbeef0009};
    u32 output[7];
    for(int i=0;i<7;i++) output[i]=0xcafecafe;
    for(int i=0;i<1024*512;i++) bitmap[i]=0x1234;
    LoadImage(&transfer,pixels);
    for(int y=0;y<3;y++) for(int x=0;x<3;x++)
        assert(bitmap[((511+y)&511)*1024+((1023+x)&1023)]==1+y*3+x);
    assert(bitmap[1*1024+2]==0x1234); /* upload padding is discarded */
    StoreImage(&transfer,output+1);
    assert(output[0]==0xcafecafe && output[6]==0xcafecafe);
    for(int i=0;i<4;i++) assert(output[i+1]==pixels[i]);
    assert(output[5]==9); /* high halfword must be zero */
    for(int i=0;i<1024*512;i++) bitmap[i]=0x1234;
    DRAWENV env={0};
    SetDefDrawEnv(&env,640,256,112,112);
    env.isbg=1;env.r0=env.g0=env.b0=8;
    PutDrawEnv(&env);check_clear(640,256,112,112);
    assert(env.dr_env.code[6]==0x60080808);
    for(int i=0;i<1024*512;i++) bitmap[i]=0x1234;
    SetDefDrawEnv(&env,320,0,320,218);
    env.isbg=1;env.r0=env.g0=env.b0=8;
    PutDrawEnv(&env);check_clear(320,0,320,218);
    assert(env.dr_env.code[6]==0x02080808);
    for(int i=0;i<1024*512;i++) bitmap[i]=0x1234;
    /* Saved effect environment: negative width must clamp to zero. The old
     * absolute fill masked it to a large positive width and erased textures. */
    env.clip=(RECT){200,520,(s16)0xfb98,520};env.ofs[0]=880;env.ofs[1]=540;
    PutDrawEnv(&env);check_clear(0,0,0,0);
    assert(env.dr_env.code[6]==0x60080808);
    return 0;
}
