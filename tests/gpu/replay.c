/* Replay normalized GPUDUMP commands through production GPU entry points.
 * Input records: type, word count, words (LE32); type 0 = complete GP0 command,
 * 1 = GP1 words, 2 = snapshot, 3/4 = read/discard count. Read results precede
 * the final VRAM snapshot on stdout. Python validates boundaries first.
 * This is a non-interlaced raster probe, not a GPU timing emulator. */
#include <stdio.h>
#include <stdlib.h>
#undef NULL
#include "psyq_port.h"
#include "gp0.h"
#include "psx_raster.h"

unsigned char psx_ram[PSX_RAM_SIZE];
extern u16 *psx_vram;
static u16 bitmap[1024*512];
static u32 read_buffer[1024*512/2];
void port_log(const char *format, ...) { (void)format; }
void port_fatal(const char *format, ...) { (void)format; abort(); }

int main(void) {
    u32 header[2];unsigned read_count=0,read_position=0;
    u32 read_latch=0;int query_latched=0;
    psx_vram=bitmap;gp0_backend=&psx_softgpu;
    for (;;) {
        size_t bytes=fread(header,1,8,stdin);
        if (!bytes) break;
        if (bytes!=8) return 2;
        unsigned type=header[0],n=header[1];
        if (n>262147) return 2;
        u32 *p=calloc(n+1,sizeof *p);
        if (!p || fread(p+1,4,n,stdin)!=n) return 2;
        if (type==0 && n) {
            unsigned cmd=p[1]>>24;
            if (cmd>=0xa0 && cmd<0xc0) {
                if (n<3) return 2;
                RECT r={(s16)(p[2]&1023),(s16)((p[2]>>16)&511),
                    (s16)(((p[3]-1)&1023)+1),(s16)((((p[3]>>16)-1)&511)+1)};
                if (n!=3+((unsigned)r.w*r.h+1)/2) return 2;
                LoadImage(&r,p+4);
            } else if (cmd>=0xc0 && cmd<0xe0) {
                if (n!=3) return 2;
                RECT r={(s16)(p[2]&1023),(s16)((p[2]>>16)&511),
                    (s16)(((p[3]-1)&1023)+1),(s16)((((p[3]>>16)-1)&511)+1)};
                read_count=((unsigned)r.w*r.h+1)/2;read_position=0;query_latched=0;
                /* Sentinels expose incomplete StoreImage writes, including
                 * the high halfword of an odd-sized transfer. */
                for(unsigned i=0;i<read_count;i++) read_buffer[i]=0xdeadbeef;
                StoreImage(&r,read_buffer);
            } else if (n<=255) {
                p[0]=n<<24;DrawPrim(p);
            } else return 2;
        } else if (type==1) {
            for (unsigned i=1;i<=n;i++) {
                unsigned cmd=p[i]>>24;
                if (cmd==0) gp0_state=(Gp0State){0};
                if (cmd==0) {read_latch=0;query_latched=0;}
                if (cmd==0 || cmd==1) read_count=read_position=0;
                if (cmd>=0x10 && cmd<0x20) {
                    /* Exercise the production snapshot API, then expose the
                     * hardware's latched GPUREAD register-query format. */
                    DR_ENV env={0};GetDrawEnv2(&env);query_latched=1;
                    switch(p[i]&15) {
                    case 2: read_latch=env.code[4]&0xfffff;break;
                    case 3: read_latch=env.code[0]&0xfffff;break;
                    case 4: read_latch=env.code[1]&0xfffff;break;
                    case 5: read_latch=env.code[2]&0x3fffff;break;
                    case 7: read_latch=2;break; /* v2 reference */
                    case 8: read_latch=0;break;
                    default: break; /* unspecified indices retain the latch */
                    }
                }
                /* Display/control commands don't affect non-interlaced VRAM.
                 * GP1(01) stream cancellation is handled by the parser. */
                if (cmd==8 && (p[i]&0x20)) return 2;
            }
        } else if (type==2 && n==0) {
            if (fwrite(bitmap,1,sizeof bitmap,stdout)!=sizeof bitmap) return 2;
        } else if ((type==3 || type==4) && n==1) {
            if(query_latched) {
                for(unsigned i=0;i<p[1];i++) if(fwrite(&read_latch,4,1,stdout)!=1) return 2;
            } else {
                if (p[1]>read_count-read_position) return 2;
                if (fwrite(read_buffer+read_position,4,p[1],stdout)!=p[1]) return 2;
                read_position+=p[1];
                if(p[1]) read_latch=read_buffer[read_position-1];
            }
        } else return 2;
        free(p);
    }
    return !feof(stdin) || ferror(stdin) || ferror(stdout);
}
