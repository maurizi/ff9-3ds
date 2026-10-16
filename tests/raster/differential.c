/* Compare entire VRAM after mixed clipped, textured and blended primitives.
 * Both backends start with identical data, including texture/CLUT feedback. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "gp0.h"

static uint16_t actual[1024*512], expected[1024*512];
uint16_t *psx_vram;
Gp0State gp0_state;
const Gp0Backend *gp0_backend;
extern const Gp0Backend psx_softgpu, psx_softgpu_reference;
#include "interpolation_golden.h"
static uint32_t seed = 0x17932761;
static unsigned random_value(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static void draw(const Gp0Backend *backend, const Gp0Mode *m, const Gp0Vtx *v, int type, int w, int h) {
    if (type < 2) backend->poly(m, v, type+3);
    else if (type == 2) backend->rect(m, v, w, h);
    else backend->line(m, v, v+1);
}
static void targeted_checks(void) {
    interpolation_checks(&psx_softgpu,actual);
    interpolation_checks(&psx_softgpu_reference,expected);
    /* Legal maximum extents with determinant +/-1 stress slope setup and
     * wrapped stepping outside extremely thin triangles under UBSan. */
    for(int reverse=0;reverse<2;reverse++) {
        memset(actual,0,sizeof actual);memset(expected,0,sizeof expected);
        Gp0Vtx v[3]={{0,0,0,255,0,255,0},{1023,511,255,0,255,0,255},
                     {1021,510,0,255,0,255,0}};
        if(reverse) {Gp0Vtx tmp=v[1];v[1]=v[2];v[2]=tmp;}
        Gp0Mode m={.gouraud=1,.textured=1,.depth=2,.raw=1,.tx=0,.ty=0};
        gp0_state=(Gp0State){.area_x1=1023,.area_y1=511};
        psx_vram=actual;psx_softgpu.poly(&m,v,3);
        psx_vram=expected;psx_softgpu_reference.poly(&m,v,3);
        assert(memcmp(actual,expected,sizeof actual)==0);
    }
    /* Synthetic 4x4 interiors verified through DuckStation's GPU. These
     * independent goldens catch colour errors shared by both native paths. */
    static const uint16_t shaded[16]={
        0x3de0,0x41e1,0x3de0,0x4201,0x4201,0x3de0,0x4201,0x41e1,
        0x3de0,0x4201,0x3de0,0x41e1,0x4201,0x41e1,0x4201,0x3de0};
    static const uint16_t modulated[16]={
        0x96a0,0x9aa0,0x96a0,0x9aa0,0x9ac1,0x96a0,0x9ac1,0x96a0,
        0x96a0,0x9aa0,0x96a0,0x9aa0,0x9ac1,0x96a0,0x9ac1,0x96a0};
    for(int py=0;py<4;py++) for(int px=0;px<4;px++) {
        memset(actual,0,sizeof actual);psx_vram=actual;
        gp0_state=(Gp0State){.area_x1=1023,.area_y1=511,.texpage=0x200};
        Gp0Mode m={.gouraud=1};
        Gp0Vtx v[4]={{px,py,9,127,129,0,0},{32+px,py,9,127,129,32,0},
                     {px,32+py,9,127,129,0,32},{32+px,32+py,9,127,129,32,32}};
        psx_softgpu.poly(&m,v,4);
        for(int y=0;y<4;y++) for(int x=0;x<4;x++)
            assert(actual[(4+y+py)*1024+4+x+px]==shaded[((y+py)&3)*4+((x+px)&3)]);
        m.gouraud=0;
        for(int y=4;y<8;y++) {
            Gp0Vtx a={0,y,9,127,129,0,0},b={31,y,9,127,129,0,0};
            psx_softgpu.line(&m,&a,&b);
        }
        for(int y=0;y<4;y++) for(int x=0;x<4;x++) assert(actual[(4+y)*1024+4+x]==shaded[y*4+x]);
        m=(Gp0Mode){.textured=1,.depth=0,.tx=640,.ty=256,.clut_y=480};
        for(int y=256;y<288;y++) for(int x=640;x<648;x++) actual[y*1024+x]=0x1111;
        actual[480*1024+1]=0x9acb;
        psx_softgpu.poly(&m,v,4);
        for(int y=0;y<4;y++) for(int x=0;x<4;x++)
            assert(actual[(4+y+py)*1024+4+x+px]==modulated[((y+py)&3)*4+((x+px)&3)]);
        m.raw=1;psx_softgpu.poly(&m,v,4);
        for(int y=4+py;y<8+py;y++) for(int x=4+px;x<8+px;x++) assert(actual[y*1024+x]==0x9acb);
        m.raw=0;psx_softgpu.rect(&m,v,32,32);
        for(int y=4+py;y<8+py;y++) for(int x=4+px;x<8+px;x++) assert(actual[y*1024+x]==0x9aa0);
    }
    /* A screen-aligned quad must cover the same top/left-inclusive,
     * bottom/right-exclusive pixels as a rectangle, including row zero. */
    for(int blend_mode=0;blend_mode<5;++blend_mode) {
        for(unsigned i=0;i<1024*512;++i) actual[i]=expected[i]=0x1234;
        Gp0Mode m={.semi=blend_mode<4,.abr=blend_mode%4};
        Gp0Vtx v[4]={{0,0,191,97,21,0,0},{96,0,191,97,21,0,0},
                     {0,96,191,97,21,0,0},{96,96,191,97,21,0,0}};
        gp0_state=(Gp0State){.area_x1=1023,.area_y1=511};
        psx_vram=actual;psx_softgpu.poly(&m,v,4);
        psx_vram=expected;psx_softgpu_reference.rect(&m,v,96,96);
        assert(memcmp(actual,expected,sizeof actual)==0);
        assert(actual[96]==0x1234 && actual[96*1024]==0x1234);
    }
    for (int depth=0; depth<2; ++depth) for (int abr=0; abr<4; ++abr)
    for (int feedback=0; feedback<2; ++feedback) for (int colour=0; colour<2; ++colour) {
        for (unsigned i=0; i<1024*512; ++i) actual[i]=0x1234;
        Gp0Mode m={.textured=1,.semi=1,.depth=depth,.abr=abr,.tx=640,.ty=256,
                   .clut_x=1008,.clut_y=feedback ? 100 : 450};
        int count=depth ? 256 : 16;
        for (int i=0;i<count;++i)
            actual[m.clut_y*1024+((m.clut_x+i)&1023)]=(i%4==0) ? 0 : (i%4==1) ? 0x8000 : (i%4==2) ? 0xFFFF : 0x7FFF;
        Gp0Vtx v={.x=0,.y=100,.r=colour ? 255 : 0,.g=colour ? 191 : 0,
                  .b=colour ? 127 : 0,.u=240,.v=240};
        gp0_state=(Gp0State){.area_x1=1023,.area_y1=511};
        memcpy(expected,actual,sizeof actual);
        psx_vram=actual; psx_softgpu.rect(&m,&v,128,80);
        psx_vram=expected; psx_softgpu_reference.rect(&m,&v,128,80);
        assert(memcmp(actual,expected,sizeof actual)==0);
    }
}
static void shadow_checks(void) {
    extern void psx_softgpu_shadow_poly(const Gp0Mode *, const Gp0Vtx *, int,
                                       uint16_t *, uint8_t *, int, int, int, int);
    extern void psx_softgpu_reference_shadow_poly(const Gp0Mode *, const Gp0Vtx *, int,
                                       uint16_t *, uint8_t *, int, int, int, int);
    extern void psx_softgpu_shadow_poly_region(const Gp0Mode *, const Gp0Vtx *, int,
                                              uint16_t *, uint8_t *, int, int, int, int,
                                              int, int, int, int);
    extern void psx_softgpu_shadow_rect(const Gp0Mode *, const Gp0Vtx *, int, int,
                                       uint16_t *, uint8_t *, int, int, int, int);
    extern void psx_softgpu_reference_shadow_rect(const Gp0Mode *, const Gp0Vtx *, int, int,
                                                 uint16_t *, uint8_t *, int, int, int, int);
    uint16_t pixels[2][64*64];
    uint8_t known[2][64*64];
    for (int depth=0;depth<2;++depth) for (int semi=0;semi<2;++semi)
    for(int mask=0;mask<4;++mask) {
        Gp0Mode m={.textured=1,.semi=semi,.depth=depth,.tx=640,.ty=256,
                   .clut_x=0,.clut_y=10};
        Gp0Vtx v[4]={{0,0,255,97,21,0,0},{64,0,255,97,21,64,0},
                     {0,64,255,97,21,0,64},{64,64,0,0,0,64,64}};
        for(unsigned i=0;i<64*64;++i) {pixels[0][i]=random_value();known[0][i]=i%2;}
        memcpy(pixels[1],pixels[0],sizeof pixels[0]);
        memcpy(known[1],known[0],sizeof known[0]);
        gp0_state=(Gp0State){.area_x1=1023,.area_y1=511,.check_mask=mask&1,.set_mask=mask>>1};
        psx_vram=actual;
        psx_softgpu_shadow_poly(&m,v,4,pixels[0],known[0],0,0,64,64);
        psx_softgpu_reference_shadow_poly(&m,v,4,pixels[1],known[1],0,0,64,64);
        assert(memcmp(pixels[0],pixels[1],sizeof pixels[0])==0);
        assert(memcmp(known[0],known[1],sizeof known[0])==0);
        for(unsigned i=0;i<64*64;++i) {pixels[0][i]=random_value();known[0][i]=i%2;}
        memcpy(pixels[1],pixels[0],sizeof pixels[0]);
        memcpy(known[1],known[0],sizeof known[0]);
        psx_softgpu_shadow_rect(&m,v,64,64,pixels[0],known[0],0,0,64,64);
        psx_softgpu_reference_shadow_rect(&m,v,64,64,pixels[1],known[1],0,0,64,64);
        assert(memcmp(pixels[0],pixels[1],sizeof pixels[0])==0);
        assert(memcmp(known[0],known[1],sizeof known[0])==0);
    }
    /* Independent clipping areas must not move a triangle's fixed-point
     * interpolation anchor. Compare disjoint, shuffled region replays with
     * one complete draw through the generic reference implementation. */
    for (int trial=0;trial<256;trial++) {
        Gp0Mode m={.textured=trial&1,.raw=(trial>>1)&1,.gouraud=(trial>>2)&1,
            .semi=(trial>>3)&1,.abr=(trial>>4)&3,.depth=trial%3,
            .tx=640,.ty=256,.clut_x=0,.clut_y=10};
        Gp0Vtx v[4];
        for (int i=0;i<4;i++) v[i]=(Gp0Vtx){.x=(int)(random_value()%96)-16,
            .y=(int)(random_value()%96)-16,.r=random_value()%256,.g=random_value()%256,
            .b=random_value()%256,.u=random_value()%256,.v=random_value()%256};
        for(unsigned i=0;i<64*64;i++) {pixels[0][i]=random_value();known[0][i]=i%3!=0;}
        memcpy(pixels[1],pixels[0],sizeof pixels[0]);
        memcpy(known[1],known[0],sizeof known[0]);
        gp0_state=(Gp0State){.area_x0=3,.area_y0=5,.area_x1=61,.area_y1=59,
                            .texpage=(trial&1)?0x200:0,
                            .set_mask=(trial>>6)&1,.check_mask=(trial>>7)&1};
        psx_softgpu_reference_shadow_poly(&m,v,3+(trial&1),pixels[1],known[1],0,0,64,64);
        for (int region=0;region<16;region++) {
            int index=region^5;
            psx_softgpu_shadow_poly_region(&m,v,3+(trial&1),pixels[0],known[0],0,0,64,64,
                                          (index%4)*16,(index/4)*16,16,16);
        }
        assert(memcmp(pixels[0],pixels[1],sizeof pixels[0])==0);
        assert(memcmp(known[0],known[1],sizeof known[0])==0);
    }
}
static void opaque_sprite_checks(void) {
    /* Large indexed palettes, neutral direct colour, STP/transparent black,
     * window remapping, clipped UV origins and live source/CLUT feedback. */
    for(int depth=0;depth<3;depth++) for(int raw=0;raw<2;raw++)
    for(int neutral=0;neutral<2;neutral++) for(int feedback=0;feedback<3;feedback++) {
        for(unsigned i=0;i<1024*512;i++)
            actual[i]=i%13==0 ? 0 : i%19==0 ? 0x8000 : random_value();
        Gp0Mode m={.textured=1,.depth=depth,.raw=raw,
            .tx=feedback==1 ? (depth==2 ? 0 : 64) : 640,
            .ty=feedback==1 ? 96 : 256,
            .clut_x=feedback==2 ? 32 : 1008,.clut_y=feedback==2 ? 104 : 450};
        Gp0Vtx v={.x=32,.y=96,.r=neutral ? 128 : 255,
            .g=neutral ? 128 : 0,.b=neutral ? 128 : 191,
            .u=feedback==1 ? 0 : 240,.v=feedback==1 ? 0 : 241};
        gp0_state=(Gp0State){.area_x0=40,.area_y0=100,.area_x1=150,.area_y1=125,
            .twin_mask_x=feedback==1 ? 0xf8 : 0x18,.twin_off_x=0x08,
            .twin_mask_y=feedback==1 ? 0 : 0x10,.set_mask=neutral};
        memcpy(expected,actual,sizeof actual);
        psx_vram=actual;psx_softgpu.rect(&m,&v,128,32);
        psx_vram=expected;psx_softgpu_reference.rect(&m,&v,128,32);
        assert(memcmp(actual,expected,sizeof actual)==0);
    }
}
static void benchmark(void) {
    Gp0Mode m={.textured=1,.depth=1,.tx=640,.ty=256,.clut_x=512,.clut_y=450};
    Gp0Vtx v[4]={{0,0,97,191,255,0,0},{96,0,97,191,255,96,0},
                 {0,96,97,191,255,0,96},{96,96,97,191,255,96,96}};
    gp0_state=(Gp0State){.area_x1=1023,.area_y1=511};
    for(unsigned i=0;i<1024*512;++i) actual[i]=random_value();
    memcpy(expected,actual,sizeof actual);
    const Gp0Backend *backends[2]={&psx_softgpu_reference,&psx_softgpu};
    double seconds[2];
    for (int b=0;b<2;++b) {
        psx_vram=b ? actual : expected;
        clock_t start=clock();
        for(int i=0;i<4000;++i) backends[b]->poly(&m,v,4);
        seconds[b]=(double)(clock()-start)/CLOCKS_PER_SEC;
    }
    assert(memcmp(actual,expected,sizeof actual)==0);
    printf("flat indexed quad CPU benchmark: generic %.3fs, optimised %.3fs (%.2fx); host only\n",
           seconds[0],seconds[1],seconds[0]/seconds[1]);
}
int main(int argc, char **argv) {
    (void)argv;
    targeted_checks();
    shadow_checks();
    opaque_sprite_checks();
    for (unsigned i=0; i<1024*512; ++i)
        actual[i] = (i % 17 == 0) ? 0 : (i % 19 == 0) ? 0x8000 : random_value();
    memcpy(expected, actual, sizeof actual);
    for (unsigned trial=0; trial<4096; ++trial) {
        Gp0Mode m = {.textured=random_value()%2, .raw=random_value()%2,
                    .semi=random_value()%2, .gouraud=random_value()%2,
                    .abr=random_value()%4, .depth=random_value()%3,
                    .tx=(random_value()%16)*64, .ty=(random_value()%2)*256,
                    .clut_x=(random_value()%64)*16, .clut_y=random_value()%512};
        int x=random_value()%1056-16, y=random_value()%544-16;
        Gp0Vtx v[4];
        for (int i=0; i<4; ++i)
            v[i]=(Gp0Vtx){x+(int)(random_value()%90),y+(int)(random_value()%90),
                          random_value()%256,random_value()%256,random_value()%256,
                          random_value()%256,random_value()%256};
        /* Flat quads deliberately have different vertex colours: triangle 2
         * must use its own strip vertex, not a cached colour from vertex 0. */
        gp0_state=(Gp0State){.area_x0=0,.area_y0=0,.area_x1=1023,.area_y1=511,
            .texpage=(trial&1)?0x200:0,
            .twin_mask_x=(random_value()%32)*8,.twin_mask_y=(random_value()%32)*8,
            .twin_off_x=(random_value()%32)*8,.twin_off_y=(random_value()%32)*8,
            .check_mask=random_value()%2,.set_mask=random_value()%2};
        if (trial%4 == 0) {
            gp0_state.area_x0=x+20; gp0_state.area_y0=y+20;
            gp0_state.area_x1=x+65; gp0_state.area_y1=y+65;
        }
        unsigned type=trial%4;
        int w=random_value()%100, h=random_value()%100;
        psx_vram=actual; draw(&psx_softgpu,&m,v,type,w,h);
        psx_vram=expected; draw(&psx_softgpu_reference,&m,v,type,w,h);
        if (memcmp(actual,expected,sizeof actual)) {
            for (unsigned i=0;i<1024*512;++i) if(actual[i]!=expected[i]) {
                fprintf(stderr,"trial %u type %u flat %d pixel %u,%u: %04x != %04x\n",
                        trial,type,!m.gouraud,i%1024,i/1024,actual[i],expected[i]);
                return 1;
            }
        }
    }
    puts("software raster: 4096 mixed primitives match the generic path byte-for-byte");
    if (argc > 1) benchmark();
    return 0;
}
