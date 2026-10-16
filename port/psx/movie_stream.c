/* FF9's reverse-chunk STR/MBG framing, following ovl_64's CD ring reader
 * (func_8019BA70 / func_8019C434). No I/O, allocation or decoder state. */
#include "movie_stream.h"
#include <string.h>

static unsigned le16(const uint8_t *p) { return p[0]|((unsigned)p[1]<<8); }
static uint32_t le32(const uint8_t *p) { return le16(p)|((uint32_t)le16(p+2)<<16); }

void ff9_movie_frame_reset(Ff9MovieFrame *f) { memset(f,0,sizeof *f); }

static int fail(Ff9MovieFrame *f, int error) { return f->error=error; }

int ff9_movie_audio_info(const uint8_t s[2048], Ff9MovieAudio *info) {
    *info=(Ff9MovieAudio){0};
    if (!s[0x10]) return FF9_MOVIE_AUDIO_NONE;
    if (s[0x10]!=1) return FF9_MOVIE_BAD_AUDIO;
    if (memcmp(s+0x80,"AKAO",4)) {
        /* MBG116 on disc 4 ends with an AB-filled sound payload. It is not
         * an AKAO block with an enormous length/pitch. Keep camera data. */
        for (unsigned i=0x80;i<2048;i++) if (s[i]!=0xab) return FF9_MOVIE_BAD_AUDIO;
        return FF9_MOVIE_AUDIO_END;
    }
    uint32_t bytes=le32(s+0xa0), pitch=le32(s+0x9c);
    if (bytes>2048-0xd0 || (bytes&15) || !pitch || pitch>0x3fff)
        return FF9_MOVIE_BAD_AUDIO;
    *info=(Ff9MovieAudio){s+0xd0,le32(s+0x84),(uint16_t)bytes,(uint16_t)pitch};
    return FF9_MOVIE_AUDIO_DATA;
}

int ff9_movie_frame_push(Ff9MovieFrame *f, const uint8_t *s, size_t bytes) {
    if (f->error) return f->error;
    if (bytes<32) return fail(f,FF9_MOVIE_BAD_SIZE);
    unsigned index=le16(s+4), type=le16(s+2);
    uint32_t number=le32(s+8);
    if (le16(s)!=0x160 || le16(s+6)!=10 || !number || index>=10)
        return fail(f,FF9_MOVIE_BAD_HEADER);
    if (f->next_sector==10) {
        if (index || number!=f->number+1) return fail(f,FF9_MOVIE_BAD_ORDER);
        f->next_sector=0;
    }
    if (index!=f->next_sector || (index && number!=f->number))
        return fail(f,FF9_MOVIE_BAD_ORDER);
    if (!index) f->number=number;
    if (index<2) {
        if (type!=8) return fail(f,FF9_MOVIE_BAD_HEADER);
        if (bytes!=2048) return fail(f,FF9_MOVIE_BAD_SIZE);
        Ff9MovieAudio audio;
        if (ff9_movie_audio_info(s,&audio)<0) return fail(f,FF9_MOVIE_BAD_AUDIO);
        memcpy(f->audio[index],s,2048);
    } else {
        if (type!=4) return fail(f,FF9_MOVIE_BAD_HEADER);
        if (bytes!=32+FF9_MOVIE_CHUNK_BYTES) return fail(f,FF9_MOVIE_BAD_SIZE);
        uint32_t words=le32(s+12);
        unsigned width=le16(s+16), height=le16(s+18);
        if (words<2 || words>sizeof f->video/4 || !width || !height || width>1024 || height>512)
            return fail(f,FF9_MOVIE_BAD_VIDEO);
        if (index==2) {
            f->video_bytes=words*4;
            f->width=width; f->height=height;
        } else if (words*4!=f->video_bytes || width!=f->width || height!=f->height ||
                   memcmp(f->video_header[0]+20,s+20,8))
            return fail(f,FF9_MOVIE_BAD_VIDEO);
        memcpy(f->video_header[index-2],s,32);
        memcpy(f->video+(9-index)*FF9_MOVIE_CHUNK_BYTES,s+32,FF9_MOVIE_CHUNK_BYTES);
    }
    f->next_sector++;
    if (f->next_sector!=10) return FF9_MOVIE_MORE;
    /* Sector 9 begins the BS stream; its first eight bytes are duplicated
     * in every video header. Retain the complete padded tail for MBG extras. */
    if (memcmp(f->video,f->video_header[0]+20,8)) return fail(f,FF9_MOVIE_BAD_VIDEO);
    return FF9_MOVIE_READY;
}
