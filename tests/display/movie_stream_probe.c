/* Run the production assembler against a complete raw-disc movie extent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_stream.h"
#include "movie_vlc.h"
static Ff9MovieFrame frame;
static unsigned char raw[2352];
static unsigned char ac_table[216];
static uint16_t coefficients[0x8002];
static unsigned get16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static unsigned get32(const unsigned char *p) {return get16(p)|(get16(p+2)<<16);}
int main(int argc,char **argv) {
    int vlc=argc==6 && !strcmp(argv[4],"--vlc-table");
    if (argc!=4 && argc!=5 && !vlc) return 2;
    if (vlc) {
        FILE *table=fopen(argv[5],"rb");if (!table) return 2;
        int ok=fread(ac_table,1,sizeof ac_table,table)==sizeof ac_table;
        ok&=fclose(table)==0;if (!ok) return 2;
    }
    FILE *f=fopen(argv[1],"rb");if (!f) return 2;
    unsigned long start=strtoul(argv[2],NULL,0), sectors=strtoul(argv[3],NULL,0);
    if (!sectors || sectors%10 || fseek(f,(long)(start*2352),SEEK_SET)) return 2;
    unsigned frames=0,first=0,max_video=0,max_rl=0,metadata=0,audio_frames=0;
    unsigned width=0,height=0,dimensions_changed=0,versions=0,declared=0,max_audio=0,padding=0,audio_end=0;
    unsigned vlc_frames=0,max_bits=0;
    for (unsigned long i=0;i<sectors;i++) {
        if (fread(raw,1,sizeof raw,f)!=sizeof raw) return 3;
        if (raw[15]!=2 || memcmp(raw+16,raw+20,4)) {
            fprintf(stderr,"bad Mode 2 sector at LBA %lu\n",start+i);return 4;
        }
        /* Some ISO extents reserve more sectors than the actual movie. The
         * audio header supplies its end; verify the unused frame-zero tail
         * separately instead of submitting it as playable frame data. */
        if (declared && frames==declared) {
            if (get16(raw+24)!=0x160 || get16(raw+26)!=4 || get16(raw+28)!=0 ||
                get16(raw+30)!=10 || get32(raw+32)!=0 || get32(raw+36)!=0) {
                fprintf(stderr,"unexpected data after declared movie end at LBA %lu\n",start+i);return 9;
            }
            padding++;continue;
        }
        int result=ff9_movie_frame_push(&frame,raw+24,(raw[18]&0x20) ? 2324 : 2048);
        if (result<0) {
            fprintf(stderr,"movie framing error %d at LBA %lu (index %u, frame %u)\n",
                    result,start+i,get16(raw+28),get32(raw+32));return 5;
        }
        if (result!=FF9_MOVIE_READY) continue;
        if (vlc) {
            Ff9MovieVlcInfo info;
            int status=ff9_movie_vlc_v2(frame.video,frame.video_bytes,ac_table,coefficients,
                                      sizeof coefficients/sizeof *coefficients,&info);
            unsigned blocks=((frame.width+15)/16)*((frame.height+15)/16)*6;
            if (status || info.blocks!=blocks) {
                fprintf(stderr,"VLC error %d (blocks %u/%u) in frame %u\n",
                        status,info.blocks,blocks,frame.number);return 10;
            }
            vlc_frames++;
            if (info.consumed_bits>max_bits) max_bits=info.consumed_bits;
        }
        if (!frames) {first=frame.number;width=frame.width;height=frame.height;}
        dimensions_changed+=frame.width!=width || frame.height!=height;
        frames++;
        if (frame.video_bytes>max_video) max_video=frame.video_bytes;
        unsigned rl=get16(frame.video);
        if (rl>max_rl) max_rl=rl;
        unsigned version=get16(frame.video+6);
        if (version>31) return 6;
        versions|=1u<<version;
        metadata+=get16(frame.audio[1]+0x12)!=0;
        declared=get32(frame.audio[1]+0x60);
        for (unsigned channel=0;channel<2;channel++) {
            Ff9MovieAudio info;
            int kind=ff9_movie_audio_info(frame.audio[channel],&info);
            if (channel==1) {
                audio_frames+=kind==FF9_MOVIE_AUDIO_DATA;
                audio_end+=kind==FF9_MOVIE_AUDIO_END;
            }
            if (info.bytes>max_audio) max_audio=info.bytes;
        }
        if (frames==1 && argc==5) {
            FILE *dump=fopen(argv[4],"wbx");if (!dump) return 7;
            int ok=fwrite(frame.video,1,frame.video_bytes,dump)==frame.video_bytes;
            ok&=fclose(dump)==0;if (!ok) return 7;
        }
    }
    fclose(f);
    printf("{\"frames\":%u,\"first_frame\":%u,\"last_frame\":%u,\"declared_frames\":%u,"
           "\"width\":%u,\"height\":%u,\"dimensions_changed\":%u,\"version_bits\":%u,"
           "\"max_bs_bytes\":%u,\"max_rl_words\":%u,\"camera_frames\":%u,"
           "\"audio_frames\":%u,\"audio_end_frames\":%u,\"max_audio_bytes\":%u,\"padding_sectors\":%u,"
           "\"vlc_frames\":%u,\"max_vlc_bits\":%u,\"frame_storage_bytes\":%zu}\n",
           frames,first,frame.number,declared,width,height,dimensions_changed,versions,
           max_video,max_rl,metadata,audio_frames,audio_end,max_audio,padding,vlc_frames,max_bits,sizeof frame);
    return frame.next_sector==10 ? 0 : 8;
}
