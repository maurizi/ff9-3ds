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
#include "overlap_reference.h"
void port_log(const char *format, ...) { (void)format; }
void port_fatal(const char *format, ...) { (void)format; abort(); }

static void check_clear(int x, int y, int w, int h) {
    for (int j=0;j<512;j++) for (int i=0;i<1024;i++)
        assert(bitmap[j*1024+i] == (i>=x && i<x+w && j>=y && j<y+h ? 0x421 : 0x1234));
}
static void check_transfer_masks(void) {
    const u16 old[4]={0x8001,2,0x8003,4};
    const u16 copy_source[4]={1,0x8002,3,0x8004},copy_old[4]={0x8005,6,0x8007,8};
    /* Upload goldens agree with the Vulkan reference; copy goldens agree
     * with software. See the documented scalar-upload / hardware-copy
     * reference bugs before replacing either table with one backend. */
    const u16 upload_expected[4][4]={{0x11,0x12,0x8013,0x8014},{0x8011,0x8012,0x8013,0x8014},
        {0x8001,0x12,0x8003,0x8014},{0x8001,0x8012,0x8003,0x8014}};
    const u16 copy_expected[4][4]={{1,0x8002,3,0x8004},{0x8001,0x8002,0x8003,0x8004},
        {0x8005,0x8002,0x8007,0x8004},{0x8005,0x8002,0x8007,0x8004}};
    const u16 identity_expected[2][4]={{1,0x8002,3,0x8004},{0x8001,0x8002,0x8003,0x8004}};
    u32 incoming[2]={0x00120011,0x80148013};
    RECT dst={100,20,4,1},src={200,20,4,1};
    for(int flags=0;flags<4;flags++) {
        gp0_state=(Gp0State){.set_mask=flags&1,.check_mask=(flags>>1)&1};
        for(int x=0;x<4;x++) bitmap[20*1024+100+x]=old[x];
        LoadImage(&dst,incoming);
        for(int x=0;x<4;x++) assert(bitmap[20*1024+100+x]==upload_expected[flags][x]);
        for(int x=0;x<4;x++) {bitmap[20*1024+200+x]=copy_source[x];bitmap[20*1024+100+x]=copy_old[x];}
        MoveImage(&src,100,20);
        for(int x=0;x<4;x++) assert(bitmap[20*1024+100+x]==copy_expected[flags][x]);
        MoveImage(&src,200,20); /* force-mask must affect identity copies too */
        for(int x=0;x<4;x++) assert(bitmap[20*1024+200+x]==identity_expected[flags&1][x]);
    }
    gp0_state=(Gp0State){.area_x1=1023,.area_y1=511};
    bitmap[20*1024+200]=0x1234;bitmap[20*1024+220]=0;
    u32 copy[]={5u<<24,0xe6000001,0x80000000,200|(20<<16),220|(20<<16),0x10001};
    DrawPrim(copy);assert(bitmap[20*1024+220]==0x9234);
    /* All commands inside a packet retain order: draw, copy, then clear.
     * The old prepass copied/cleared before any primitive was rasterized. */
    bitmap[10*1024+20]=bitmap[10*1024+64]=0;
    u32 mixed[]={10u<<24,0xe6000000,0x680000f8,20|(10<<16),
        0x80000000,20|(10<<16),64|(10<<16),0x10001,
        0x02000000,16|(10<<16),0x10010};
    DrawPrim(mixed);
    assert(bitmap[10*1024+20]==0 && bitmap[10*1024+64]==0x001f);
    gp0_state=(Gp0State){0};
    /* Zero GPU dimensions encode the maximum extent, unlike an empty RECT
     * passed to the high-level MoveImage API. */
    for(int x=0;x<1024;x++) {bitmap[100*1024+x]=0x1000+x;bitmap[101*1024+x]=0;}
    u32 full_width[]={4u<<24,0x80000000,100u<<16,101u<<16,0x10000};
    DrawPrim(full_width);
    for(int x=0;x<1024;x++) assert(bitmap[101*1024+x]==0x1000+x);
    for(int y=0;y<512;y++) {bitmap[y*1024+350]=0x2000+y;bitmap[y*1024+351]=0;}
    u32 full_height[]={4u<<24,0x80000000,350,351,1};
    DrawPrim(full_height);
    for(int y=0;y<512;y++) assert(bitmap[y*1024+351]==0x2000+y);
}
int main(void) {
    psx_vram=bitmap;gp0_backend=&psx_softgpu;
    check_overlap_reference();
    check_transfer_masks();
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
