/* Normal C interface for FF9's BS v2 expansion. Prefix groups and table
 * indices follow ovl_64 mbg_vlcDecode / mbg_vlcDecodeBlock, including escapes.
 * Unlike the original register-ABI routine, every input/output is bounded. */
#include "movie_vlc.h"

typedef struct { const uint8_t *data; unsigned bytes, bit; } Bits;
static unsigned le16(const uint8_t *p) {return p[0]|((unsigned)p[1]<<8);}
static uint32_t halfword(const Bits *b, unsigned offset) {
    return offset+1<b->bytes ? le16(b->data+offset) : 0;
}
static uint32_t peek(const Bits *b) {
    unsigned offset=(b->bit/16)*2, shift=b->bit&15;
    uint32_t word=(halfword(b,offset)<<16)|halfword(b,offset+2);
    return shift ? (word<<shift)|(halfword(b,offset+4)>>(16-shift)) : word;
}
static int consume(Bits *b, unsigned n) {
    if (n>b->bytes*8-b->bit) return 0;
    b->bit+=n;return 1;
}

static int ac(Bits *b, const uint8_t *pairs, uint16_t *value, int *eob) {
    *eob=0;
    uint32_t v=peek(b);
    unsigned bits,offset=0;
    int table=0;
    if (v&0x80000000u) {
        if (!(v&0x40000000u)) {*value=0xfe00;bits=2;*eob=1;}
        else {*value=(v&0x20000000u) ? 0x3ff : 1;bits=3;}
    } else if (v&0x40000000u) {
        if (v&0x20000000u) {*value=(v&0x10000000u) ? 0x7ff : 0x401;bits=4;}
        else {
            unsigned run=(v&0x10000000u) ? 2 : 0, level=run ? 1 : 2;
            *value=(run<<10)|((v&0x08000000u) ? (1024-level) : level);bits=5;
        }
    } else if (v&0x20000000u) {
        table=1;
        if (v&0x18000000u) {offset=0x19c+((v>>25)&14);bits=6;}
        else {offset=0x180+((v>>22)&30);bits=9;}
    } else if ((v>>26)==1) {
        *value=(v>>10)&0xffff;bits=22;
    } else {
        unsigned zeros=v ? (unsigned)__builtin_clz(v) : 32;
        if (zeros<3 || zeros==5 || zeros>11)
            return b->bytes*8-b->bit<17 ? FF9_VLC_TRUNCATED : FF9_VLC_CODE;
        table=1;
        uint32_t shifted=v<<zeros;
        if (zeros<5) {offset=(zeros-3)*16+((shifted>>27)&14);bits=zeros+4;}
        else if (zeros==6) {offset=0x20+((shifted>>26)&30);bits=11;}
        else {offset=(zeros-6)*64+((shifted>>25)&62);bits=zeros+6;}
    }
    if (!consume(b,bits)) return FF9_VLC_TRUNCATED;
    if (table) {
        unsigned index=offset/4;
        if (index>=108) return FF9_VLC_CODE;
        unsigned run=pairs[index*2],level=pairs[index*2+1];
        if (run>63 || !level) return FF9_VLC_CODE;
        *value=(run<<10)|((offset&2) ? ((1024-level)&1023) : level);
    }
    return FF9_VLC_OK;
}

int ff9_movie_vlc_v2(const uint8_t *bs, size_t bytes, const uint8_t pairs[216],
                     uint16_t *out, size_t capacity, Ff9MovieVlcInfo *info) {
    *info=(Ff9MovieVlcInfo){0};
    if (bytes<8 || bytes>18336 || (bytes&1) || le16(bs+2)!=0x3800 ||
        le16(bs+6)!=2 || le16(bs+4)>63 || !le16(bs) || le16(bs)>0x4000)
        return FF9_VLC_HEADER;
    unsigned total=le16(bs)*2+2, used=2, blocks=0;
    if (capacity<total) return FF9_VLC_CAPACITY;
    out[0]=le16(bs);out[1]=le16(bs+2);
    Bits b={bs+8,(unsigned)bytes-8,0};
    for (;;) {
        unsigned dc=peek(&b)>>22;
        if (!consume(&b,10)) return FF9_VLC_TRUNCATED;
        if (dc==0x1ff) break;
        if (used>=total) return FF9_VLC_CAPACITY;
        out[used++]=(le16(bs+4)<<10)|dc;
        for (;;) {
            uint16_t coefficient;
            int end;
            int result=ac(&b,pairs,&coefficient,&end);
            if (result) return result;
            if (used>=total) return FF9_VLC_CAPACITY;
            out[used++]=coefficient;
            if (end) break;
        }
        blocks++;
    }
    while (used<total) out[used++]=0xfe00;
    *info=(Ff9MovieVlcInfo){total,blocks,b.bit};
    return FF9_VLC_OK;
}
