#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "movie_stream.h"
static Ff9MovieFrame frame;
static unsigned char sector[2324];
static void put16(unsigned char *p,unsigned v) {p[0]=v;p[1]=v>>8;}
static void put32(unsigned char *p,unsigned v) {put16(p,v);put16(p+2,v>>16);}
static unsigned make_sector(unsigned index,unsigned number) {
    memset(sector,0,sizeof sector);
    put16(sector,0x160);put16(sector+2,index<2 ? 8 : 4);
    put16(sector+4,index);put16(sector+6,10);put32(sector+8,number);
    if (index<2) {
        memset(sector+0x14,index+number,0x24*2); /* retain both cameras */
        return 2048;
    }
    put32(sector+12,1000);put16(sector+16,320);put16(sector+18,224);
    for (unsigned i=0;i<8;i++) sector[20+i]=0x40+i;
    memset(sector+32,index,FF9_MOVIE_CHUNK_BYTES);
    if (index==9) memcpy(sector+32,sector+20,8);
    return sizeof sector;
}
static void feed(unsigned first,unsigned last,unsigned number) {
    for (unsigned i=first;i<=last;i++) {
        unsigned size=make_sector(i,number);
        assert(ff9_movie_frame_push(&frame,sector,size)==(i==9 ? FF9_MOVIE_READY : FF9_MOVIE_MORE));
    }
}
int main(void) {
    ff9_movie_frame_reset(&frame);feed(0,9,47);
    assert(frame.number==47 && frame.video_bytes==4000 && frame.width==320 && frame.height==224);
    for (unsigned chunk=0;chunk<8;chunk++)
        for (unsigned i=chunk ? 0 : 8;i<FF9_MOVIE_CHUNK_BYTES;i++)
            assert(frame.video[chunk*FF9_MOVIE_CHUNK_BYTES+i]==9-chunk);
    assert(frame.audio[0][0x14]==47 && frame.audio[1][0x14]==48);
    feed(0,9,48); /* bounded reuse; no stale sector data */
    unsigned size=make_sector(0,50);
    assert(ff9_movie_frame_push(&frame,sector,size)==FF9_MOVIE_BAD_ORDER);
    make_sector(0,49);assert(ff9_movie_frame_push(&frame,sector,size)==FF9_MOVIE_BAD_ORDER);
    ff9_movie_frame_reset(&frame);feed(0,1,1200); /* direct seek */
    size=make_sector(2,1200);put32(sector+12,0xffffffff);
    assert(ff9_movie_frame_push(&frame,sector,size)==FF9_MOVIE_BAD_VIDEO);
    ff9_movie_frame_reset(&frame);feed(0,8,1);
    size=make_sector(9,1);sector[32]^=1;
    assert(ff9_movie_frame_push(&frame,sector,size)==FF9_MOVIE_BAD_VIDEO);
    ff9_movie_frame_reset(&frame);size=make_sector(0,1);
    assert(ff9_movie_frame_push(&frame,sector,31)==FF9_MOVIE_BAD_SIZE);
    ff9_movie_frame_reset(&frame);feed(0,2,1);
    size=make_sector(3,1);put16(sector+16,256);
    assert(ff9_movie_frame_push(&frame,sector,size)==FF9_MOVIE_BAD_VIDEO);
    ff9_movie_frame_reset(&frame);size=make_sector(0,1);put16(sector+6,11);
    assert(ff9_movie_frame_push(&frame,sector,size)==FF9_MOVIE_BAD_HEADER);
    Ff9MovieAudio audio;
    make_sector(0,1);
    assert(ff9_movie_audio_info(sector,&audio)==FF9_MOVIE_AUDIO_NONE && !audio.adpcm);
    sector[0x10]=1;memcpy(sector+0x80,"AKAO",4);
    put32(sector+0x84,47);put32(sector+0x9c,0x116a);put32(sector+0xa0,0x730);
    assert(ff9_movie_audio_info(sector,&audio)==FF9_MOVIE_AUDIO_DATA);
    assert(audio.adpcm==sector+0xd0 && audio.bytes==0x730 && audio.pitch==0x116a && audio.number==47);
    put32(sector+0xa0,0xfffffff0);
    assert(ff9_movie_audio_info(sector,&audio)==FF9_MOVIE_BAD_AUDIO);
    put32(sector+0xa0,17);
    assert(ff9_movie_audio_info(sector,&audio)==FF9_MOVIE_BAD_AUDIO);
    memset(sector+0x80,0xab,2048-0x80);
    assert(ff9_movie_audio_info(sector,&audio)==FF9_MOVIE_AUDIO_END && !audio.adpcm);
    sector[2047]=0;
    assert(ff9_movie_audio_info(sector,&audio)==FF9_MOVIE_BAD_AUDIO);
    ff9_movie_frame_reset(&frame);
    assert(ff9_movie_frame_push(&frame,sector,2048)==FF9_MOVIE_BAD_AUDIO);
    puts("FF9 movie framing: reverse payloads, metadata, seeking and malformed sectors pass");
}
