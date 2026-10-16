/* Synthetic input layout adapted from JaCzekanski/ps1-tests (MIT).
 * License: ps1-tests-LICENSE.txt. Reference: gpu/vram-to-vram-overlap/vram.png
 * at 93568c5589e58b1fe442c854728324994bd30426.
 * FNV-1a over RGB15 words in 29x29 interiors excludes labels and grid lines.
 * The published PNG cannot establish bit-15 or hardware-version behavior.
 */
#ifndef GPU_OVERLAP_REFERENCE_H
#define GPU_OVERLAP_REFERENCE_H
static void check_overlap_reference(void) {
    static const unsigned expected[147]={
        0xa2d3254fu,0x7b6c886fu,0xf108e48fu,0x159a05adu,0x50ec16cdu,0x033ee28fu,0xe1db341fu,
        0x3bfd260fu,0xf128401fu,0xf3482dafu,0x57a78e27u,0xb9d4c487u,0x9eec2e1fu,0x3b32d04fu,
        0xc5e4341fu,0x48d1904fu,0x920c153du,0xf7d01785u,0xd225da4fu,0x3ae9bdafu,0x9684bd4fu,
        0x37b20bdfu,0x8d41665du,0xfaf60c61u,0x9653c78fu,0x162b5c4fu,0xfdd30265u,0x7494b021u,
        0xde73dfffu,0x0add979fu,0x65577ebfu,0xf9b1d7ffu,0x798868ffu,0x798ef01fu,0xe8ca13ffu,
        0x421541e9u,0x856cb9e5u,0x475324b1u,0x42b68b8fu,0x205b91efu,0xfd139dddu,0x00ea8157u,
        0x3b94727fu,0x070918cfu,0x983888f7u,0x475ea1bfu,0x0dfa8f7fu,0x7b77ba57u,0x34eb98dfu,
        0xc9582e47u,0xd0200d9fu,0xc48f44edu,0x2afabdc1u,0x039ca101u,0xa20cf52bu,0x9725350fu,
        0x6f886533u,0xecba7b8bu,0xb12022edu,0xc7bab37fu,0x43be7a61u,0xeffa4aabu,0xaec92613u,
        0x510c335fu,0x35589d6du,0x1b725141u,0x15b5195fu,0x824d831fu,0x61a880f5u,0xa0017411u,
        0x0c30e87fu,0x32dc563fu,0x739a80ffu,0x2833fbffu,0x9acd2a7fu,0x2e64c93fu,0x7ee09a7fu,
        0x59eef44du,0xd7c14f85u,0xb0def781u,0x39e1741fu,0x1c8a271fu,0x1b61c00du,0xd3f49e3bu,
        0x066ad16du,0x92dc2541u,0x0014085fu,0xbfef399fu,0x25aaacf5u,0x8b3ab291u,0x171b2d47u,
        0xffb1f63fu,0x19bf0bffu,0x3275fa7fu,0x2833fbffu,0x6b8a8b3fu,0xdcb4537fu,0xc197257fu,
        0xc6b14b3bu,0x1d999021u,0x99aaebffu,0x749b289fu,0xad706ecdu,0x661a2a81u,0x62dc4991u,
        0x39998edfu,0x119146dfu,0x5df1ec1fu,0xf366e09fu,0x9ba3a65fu,0x814a8b5fu,0x34b5ea5fu,
        0xc6db711fu,0x473521a5u,0x6eadfae1u,0x2833fbffu,0x9132663fu,0xd0ec8c8du,0x9ed5d881u,
        0xd0361765u,0xbd0701c5u,0xe9fe8d81u,0xe055557fu,0xd808601fu,0xd102e00du,0xc04437a3u,
        0x980f0b1fu,0x9745fcdfu,0xda9d48dfu,0xbceb909fu,0x10a92c1fu,0x4ad18f1fu,0x96632c5fu,
        0xc9f68de5u,0x271a4f21u,0xffb24abfu,0x2833fbffu,0xd68e614du,0x688b62c1u,0x75c03157u,
        0xf10a0ff3u,0x8fcf7981u,0xee62783fu,0x55c6b63fu,0xe4ef3a95u,0x912a1d51u,0xdcff8bd1u,
    };
    static const int sizes[7]={2,8,15,16,16,16,16};
    for(int p=0;p<1024*512;p++) bitmap[p]=0;
    gp0_state=(Gp0State){0};
    for(int t=0;t<7;t++) for(int cell=1;cell<=21;cell++) {
        int n=sizes[t];u32 pixels[128]={0};
        for(int y=0;y<n;y++) for(int x=0;x<n;x++) {
            int p=y*n+x;
            pixels[p/2]|=(u32)(y*64+x*2)<<((p&1)*16);
        }
        RECT r={cell*42+4,t*42+4,n,n};LoadImage(&r,pixels);
    }
    for(int t=0;t<7;t++) {
        int cell=1,n=sizes[t];
        for(int y=-1;y<=1;y++) for(int x=-3;x<=3;x++,cell++) {
            RECT src={cell*42+4+(t==4 || t==6),t*42+4+(t==5 || t==6),n,n};
            MoveImage(&src,src.x+x,src.y+y);
        }
    }
    for(int t=0;t<7;t++) for(int cell=1;cell<=21;cell++) {
        unsigned hash=2166136261u;
        for(int y=1;y<30;y++) for(int x=1;x<30;x++)
            hash=(hash^(bitmap[(t*42+y)*1024+cell*42+x]&32767))*16777619u;
        assert(hash==expected[t*21+cell-1]);
    }
}
#endif
