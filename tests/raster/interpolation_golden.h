/* Synthetic 4x4 RGB15/mask goldens from DuckStation software GPU draws.
 * These contain no game data. Regenerate/check with test_raster_psx.py;
 * see docs/render-colour-validation.md for setup and reference limits. */
#ifndef INTERPOLATION_GOLDEN_H
#define INTERPOLATION_GOLDEN_H
static void interpolation_checks(const Gp0Backend *backend, uint16_t *bitmap) {
    static const struct {
        Gp0Mode mode;
        Gp0Vtx vertices[4];
        int count, dither;
        uint16_t expected[16];
    } cases[]={
        {{.textured=0,.depth=0,.raw=0,.gouraud=1,.tx=832,.ty=256,.clut_y=481},{{0,0,233,33,7,2,1},{32,0,3,165,253,29,7},{0,32,193,216,63,9,37}},3,0,{0x1538,0x1938,0x1d57,0x2156,0x1538,0x1957,0x1d57,0x2176,0x1958,0x1d77,0x2176,0x2195,0x1978,0x1d77,0x2196,0x2595}},
        {{.textured=0,.depth=0,.raw=0,.gouraud=1,.tx=832,.ty=256,.clut_y=481},{{-9,-7,233,33,7,2,1},{37,1,3,165,253,29,7},{2,35,193,216,63,9,37}},3,1,{0x2594,0x29b4,0x29b3,0x2db3,0x25b5,0x25b4,0x2dd4,0x2dd3,0x25b5,0x29d5,0x29b3,0x2dd3,0x25d5,0x29d4,0x2df4,0x2dd3}},
        {{.textured=1,.depth=2,.raw=0,.gouraud=1,.tx=832,.ty=256,.clut_y=481},{{-9,-7,233,33,7,2,1},{37,1,3,165,253,29,7},{2,35,193,216,63,9,37}},3,1,{0x810e,0x812e,0x810e,0x812e,0x814f,0x812e,0x814f,0x914f,0x8d4e,0x8d6e,0x916e,0x816f,0x918f,0x918e,0x91af,0x818f}},
        {{.textured=1,.depth=0,.raw=1,.gouraud=1,.tx=896,.ty=256,.clut_y=481},{{-9,-7,233,33,7,2,1},{37,1,3,165,253,29,7},{2,35,193,216,63,9,37}},3,0,{0xe729,0xe729,0x838a,0x838a,0xb84c,0xb84c,0xd4ad,0xf10e,0x8d6f,0x8d6f,0xa9d0,0x8421,0xa082,0xa082,0xbce3,0xd944}},
        {{.textured=1,.depth=1,.raw=0,.gouraud=1,.tx=960,.ty=256,.clut_y=481},{{1,0,233,33,7,2,1},{32,5,3,165,253,29,7},{1,33,193,216,63,9,37}},3,1,{0x8ddf,0x981f,0xa83f,0x847f,0xa55f,0xa95f,0x89bf,0x95ff,0x95ff,0xa640,0xaa40,0x8001,0x8c43,0x9484,0xa8e6,0xb926}},
        {{.textured=1,.depth=2,.raw=0,.gouraud=0,.tx=832,.ty=256,.clut_y=481},{{8,8,233,33,7,2,1},{4,10,233,33,7,29,7},{7,3,233,33,7,9,37},{4,4,233,33,7,43,17}},4,1,{0x8095,0x80c0,0x80ff,0x8412,0x8092,0x80bf,0x84ff,0x80d2,0x806c,0x80bf,0x80be,0x8492,0x8069,0x809f,0x809f,0x8071}},
        {{.textured=1,.depth=2,.raw=1,.gouraud=1,.tx=832,.ty=256,.clut_y=481},{{8,8,233,33,7,2,1},{4,10,3,165,253,29,7},{7,3,193,216,63,9,37},{4,4,127,67,167,43,17}},4,0,{0xea4c,0xa300,0xbbd4,0xd80a,0x9a0a,0xa2de,0xb3b1,0xc72a,0xa1e7,0xbabb,0x9f11,0xee6a,0xa1a5,0xaa79,0x8651,0x95aa}},
        {{.textured=1,.depth=0,.raw=0,.gouraud=1,.tx=896,.ty=256,.clut_y=481},{{4,8,233,33,7,2,1},{8,10,3,165,253,29,7},{5,3,193,216,63,9,37},{8,4,127,67,167,43,17}},4,1,{0x14a5,0x978a,0x97ed,0xfd85,0x14a5,0x8c82,0xf285,0xe0ac,0x14a5,0x95d7,0x8ec5,0xbae5,0x14a5,0xb8d3,0xd984,0x8dc9}},
        {{.textured=1,.depth=2,.raw=0,.gouraud=1,.tx=832,.ty=256,.clut_y=481},{{32,0,3,165,253,29,7},{0,0,233,33,7,2,1},{32,32,127,67,167,43,17},{0,32,193,216,63,9,37}},4,1,{0x800d,0x8c2e,0x882f,0x9451,0x802d,0x882e,0x9050,0x9431,0x882e,0x8c50,0x8c31,0x9433,0x8c6f,0x9050,0x9052,0x9033}},
        {{.textured=1,.depth=2,.raw=1,.gouraud=1,.tx=832,.ty=256,.clut_y=481},{{2,35,193,216,63,9,37},{37,1,3,165,253,29,7},{-9,-7,233,33,7,2,1}},3,0,{0x816b,0x816b,0x856c,0x856c,0x858b,0x858b,0x818c,0x9d8d,0x99ab,0x99ab,0x9dac,0x81ad,0x9dcb,0x9dcb,0x99cc,0x85cd}},
    };
    psx_vram=bitmap;
    for (unsigned test=0;test<sizeof cases/sizeof cases[0];test++) {
        memset(bitmap,0,1024*512*sizeof *bitmap);
        for(int y=0;y<32;y++) for(int x=0;x<32;x++) bitmap[y*1024+x]=0x14a5;
        for(int v=0;v<64;v++) {
            for(int u=0;u<64;u++) bitmap[(v+256)*1024+832+u]=0x8000|((u+1)&31)|(((v+1)&31)<<5)|(((u^v)&31)<<10);
            for(int u=0;u<16;u++) for(int k=0;k<4;k++) bitmap[(v+256)*1024+896+u]|=((u*4+k+v*3)&15)<<(k*4);
            for(int u=0;u<32;u++) for(int k=0;k<2;k++) bitmap[(v+256)*1024+960+u]|=((u*2+k+v*3)&255)<<(k*8);
        }
        for(int i=0;i<256;i++) bitmap[481*1024+i]=0x8000|((i+1)&31)|(((i*3+1)&31)<<5)|(((i*7+1)&31)<<10);
        gp0_state=(Gp0State){.area_x1=1023,.area_y1=511,.texpage=cases[test].dither?512:0};
        backend->poly(&cases[test].mode,cases[test].vertices,cases[test].count);
        for(int y=4;y<8;y++) for(int x=4;x<8;x++)
            assert(bitmap[y*1024+x]==cases[test].expected[(y-4)*4+x-4]);
    }
}
#endif
