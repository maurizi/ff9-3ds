#include <stdint.h>
#include <stdio.h>
#include "mdec_decode.h"
static uint8_t tables[256],pixels[768];
static uint16_t rl[4096];
int main(void) {
    if (fread(tables,1,256,stdin)!=256) return 2;
    PsxMdecEnv env;psx_mdec_env_init(&env,tables,tables+128);
    uint32_t header[2];
    while (fread(header,1,8,stdin)==8) {
        if (header[1]>4096 || fread(rl,2,header[1],stdin)!=header[1]) return 3;
        size_t consumed;
        int32_t status=psx_mdec_macroblock(&env,rl,header[1],header[0],pixels,sizeof pixels,&consumed);
        uint32_t used=(uint32_t)consumed;
        unsigned bytes=((header[0]>>27)&3)==2 ? 768 : 512;
        if (fwrite(&status,1,4,stdout)!=4 || fwrite(&used,1,4,stdout)!=4) return 4;
        if (!status && fwrite(pixels,1,bytes,stdout)!=bytes) return 4;
    }
    return ferror(stdin) || ferror(stdout) ? 5 : 0;
}
