/* stdin: user's 216-byte AC table, then repeated {u32 size, BS bytes}.
 * stdout per frame: s32 status, u32 halfwords/blocks/bits, u16 output[]. */
#include <stdio.h>
#include <stdint.h>
#include "movie_vlc.h"
static uint8_t table[216],bs[18336];
static uint16_t output[0x8002];
int main(void) {
    if (fread(table,1,sizeof table,stdin)!=sizeof table) return 2;
    uint32_t bytes;
    while (fread(&bytes,1,4,stdin)==4) {
        if (bytes>sizeof bs || fread(bs,1,bytes,stdin)!=bytes) return 3;
        Ff9MovieVlcInfo info;
        int32_t status=ff9_movie_vlc_v2(bs,bytes,table,output,sizeof output/sizeof *output,&info);
        uint32_t stats[3]={info.halfwords,info.blocks,info.consumed_bits};
        if (fwrite(&status,1,4,stdout)!=4 || fwrite(stats,1,12,stdout)!=12) return 4;
        if (!status && fwrite(output,2,info.halfwords,stdout)!=info.halfwords) return 4;
    }
    return ferror(stdin) || ferror(stdout) ? 5 : 0;
}
