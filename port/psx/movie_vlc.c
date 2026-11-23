/* Normal C interface for FF9's BS v2 expansion. Prefix groups and table
 * indices follow ovl_64 mbg_vlcDecode / mbg_vlcDecodeBlock, including escapes.
 * Unlike the original register-ABI routine, every input/output is bounded. */
#include "movie_vlc.h"

/* (at, w: the three halfwords from byte `at` on, kept: peek's window moves to the next only every 16 bits, about every
 * third code, and building it took three bounded reads for every code) */
typedef struct { const uint8_t *data; unsigned bytes, bit, at; uint64_t w; uint32_t hi; } Bits; /* (hi: w's first 32 bits) */
static unsigned le16(const uint8_t *p) {return p[0]|((unsigned)p[1]<<8);}
static uint32_t halfword(const Bits *b, unsigned offset) {
    return offset+1<b->bytes ? le16(b->data+offset) : 0;
}
static uint32_t peek(Bits *b) {
    unsigned offset=(b->bit/16)*2, shift=b->bit&15;
    if (offset!=b->at) {
        b->at=offset;
        b->w=(uint64_t)halfword(b,offset)<<32|(uint64_t)halfword(b,offset+2)<<16|halfword(b,offset+4);
        b->hi=(uint32_t)(b->w>>16);
    }
    return (uint32_t)((b->w<<shift)>>16);
}
/* the next 12 bits (peek's top 12: the window's first 32 bits hold them at any of its 16 places) */
static inline unsigned peek12(Bits *b) {
    const unsigned offset=(b->bit/16)*2;
    if (offset!=b->at) (void)peek(b);
    return (b->hi<<(b->bit&15))>>20;
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

/* The codes of up to 12 bits by their first 12 bits: each entry is ac's own answer for its prefix, taken where ac gives
 * the same code (OK, at most 12 bits) whether the prefix is followed by zeros or by ones: so it depends on the prefix
 * alone. Anything else (longer codes, escapes, errors, too few bits left) is ac's as before. Made again when the run/
 * level pairs change. (ac's tests, every code, were most of the movies' VLC) */
#define SHORT_BITS 12
static struct { uint16_t value; uint8_t bits, eob; } short_codes[1u<<SHORT_BITS];
/* (and the codes of up to 8 bits by their first 8: 1 KB, where the 12-bit table is 16 KB, an Old 3DS's whole data
 * cache: a code's entry there is picked by the bits that follow it. A busy movie frame's expansion 7.8 -> 7.1 ms
 * there (field 2752's movie); the output the same on 200,000 random streams) */
static struct { uint16_t value; uint8_t bits, eob; } short8[256];
static uint8_t short_pairs[216];
static int short_made;
static void short_make(const uint8_t pairs[216]) {
    for (unsigned p=0;p<(1u<<SHORT_BITS);p++) {
        uint8_t z[8],o[8];
        uint16_t vz=0,vo=0;
        int ez=0,eo=0;
        const uint32_t hi=p<<(32-SHORT_BITS);
        for (int k=0;k<4;k++) { /* (the stream's halfwords little-endian: the prefix, then all zeros or all ones) */
            uint32_t wz=hi, wo=hi|((1u<<(32-SHORT_BITS))-1);
            uint16_t hz=(uint16_t)(k==0 ? wz>>16 : k==1 ? wz : 0), ho=(uint16_t)(k==0 ? wo>>16 : k==1 ? wo : 0xFFFF);
            z[k*2]=(uint8_t)hz,z[k*2+1]=(uint8_t)(hz>>8),o[k*2]=(uint8_t)ho,o[k*2+1]=(uint8_t)(ho>>8);
        }
        Bits bz={z,8,0,~0u,0},bo={o,8,0,~0u,0};
        const int rz=ac(&bz,pairs,&vz,&ez),ro=ac(&bo,pairs,&vo,&eo);
        short_codes[p].bits=0;
        if (rz==FF9_VLC_OK && ro==FF9_VLC_OK && bz.bit==bo.bit && bz.bit<=SHORT_BITS && vz==vo && ez==eo)
            short_codes[p].value=vz,short_codes[p].bits=(uint8_t)bz.bit,short_codes[p].eob=(uint8_t)ez;
    }
    for (unsigned p=0;p<256;p++) {
        short8[p].bits=0;
        if (short_codes[p<<4].bits && short_codes[p<<4].bits<=8) {
            short8[p].value=short_codes[p<<4].value,short8[p].bits=short_codes[p<<4].bits,short8[p].eob=short_codes[p<<4].eob;
        }
    }
    __builtin_memcpy(short_pairs,pairs,216);
    short_made=1;
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
    Bits b={bs+8,(unsigned)bytes-8,0,~0u,0};
    if (!short_made || __builtin_memcmp(short_pairs,pairs,216)) short_make(pairs);
    for (;;) {
        unsigned dc=peek(&b)>>22;
        if (!consume(&b,10)) return FF9_VLC_TRUNCATED;
        if (dc==0x1ff) break;
        if (used>=total) return FF9_VLC_CAPACITY;
        out[used++]=(le16(bs+4)<<10)|dc;
        for (;;) {
            uint16_t coefficient;
            int end;
            const unsigned k=peek12(&b),n8=short8[k>>4].bits,n=n8 ? n8 : short_codes[k].bits;
            if (n8 && n8<=b.bytes*8-b.bit) b.bit+=n8,coefficient=short8[k>>4].value,end=short8[k>>4].eob;
            else if (n && n<=b.bytes*8-b.bit) b.bit+=n,coefficient=short_codes[k].value,end=short_codes[k].eob;
            else {
                int result=ac(&b,pairs,&coefficient,&end);
                if (result) return result;
            }
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
