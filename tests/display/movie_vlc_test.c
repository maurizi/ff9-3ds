#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "movie_vlc.h"
static uint8_t table[216],bs[128];
static uint16_t output[0x8003];
static unsigned bit;
static void begin(void) {
    memset(bs,0,sizeof bs);bs[0]=16;bs[3]=0x38;bs[4]=12;bs[6]=2;bit=0;
    memset(output,0xa5,sizeof output);
}
static void emit(unsigned value,unsigned count) {
    while (count--) {
        unsigned offset=8+(bit/16)*2;
        uint16_t w=bs[offset]|((uint16_t)bs[offset+1]<<8);
        w|=((value>>count)&1)<<(15-(bit&15));
        bs[offset]=w;bs[offset+1]=w>>8;bit++;
    }
}
static void check(unsigned code,unsigned bits,uint16_t expected) {
    begin();emit(0x155,10);emit(code,bits);emit(2,2);emit(0x1ff,10);
    Ff9MovieVlcInfo info;
    assert(ff9_movie_vlc_v2(bs,sizeof bs,table,output,0x8002,&info)==0);
    assert(info.halfwords==34 && info.blocks==1 && info.consumed_bits==10+bits+2+10);
    assert(output[0]==16 && output[1]==0x3800 && output[2]==(12*1024+0x155));
    assert(output[3]==expected && output[4]==0xfe00);
    for (unsigned i=5;i<34;i++) assert(output[i]==0xfe00);
    assert(output[34]==0xa5a5);
}
int main(void) {
    for (unsigned i=0;i<108;i++) {table[i*2]=i%63;table[i*2+1]=i%31+1;}
    /* Encode each prefix independently of the production lookup arithmetic. */
    const struct {unsigned prefix,bits,index_bits,first;} groups[]={
        {1,4,2,0},{1,5,2,4},{1,7,3,8},{1,8,4,16},{1,9,4,32},
        {1,10,4,48},{1,11,4,64},{1,12,4,80},{4,5,3,96},
        {5,5,0,104},{6,5,0,105},{7,5,0,106}};
    for (unsigned g=0;g<sizeof groups/sizeof groups[0];g++)
    for (unsigned index=0;index<(1u<<groups[g].index_bits);index++)
    for (unsigned sign=0;sign<2;sign++) {
        unsigned entry=groups[g].first+index;
        unsigned run=table[entry*2],level=table[entry*2+1];
        unsigned code=(((groups[g].prefix<<groups[g].index_bits)|index)<<1)|sign;
        check(code,groups[g].bits+groups[g].index_bits+1,
              (run<<10)|(sign ? 1024-level : level));
    }
    check(6,3,1);check(7,3,0x3ff);
    check(6,4,0x401);check(7,4,0x7ff);
    check(8,5,2);check(9,5,0x3fe);check(10,5,0x801);check(11,5,0xbff);
    check((1u<<16)|0x3e2,22,0x3e2); /* FF9's escaped level -30 */
    check((1u<<16)|0xfe00,22,0xfe00); /* raw value does not consume the next EOB */
    Ff9MovieVlcInfo info;
    assert(ff9_movie_vlc_v2(bs,sizeof bs,table,output,33,&info)==FF9_VLC_CAPACITY);
    assert(info.halfwords==0);
    assert(ff9_movie_vlc_v2(bs,9,table,output,0x8002,&info)==FF9_VLC_HEADER);
    assert(ff9_movie_vlc_v2(bs,10,table,output,0x8002,&info)==FF9_VLC_TRUNCATED);
    bs[6]=3;assert(ff9_movie_vlc_v2(bs,sizeof bs,table,output,0x8002,&info)==FF9_VLC_HEADER);
    puts("FF9 VLC v2: all AC prefix/sign groups, escapes, padding and bounds pass");
    return 0;
}
