#include <nds.h>
#include <stdio.h>
#include <string.h>
#include "movie_vlc.h"
int movie_vlc_tests(void);
extern const unsigned char movie_vlc_ac_table[];
extern const unsigned movie_vlc_fixture_count;
extern const struct {
    const unsigned char *bs;
    unsigned bytes;
    const unsigned short *reference;
    unsigned reference_bytes,blocks;
} movie_vlc_fixtures[];
static uint16_t output[0x8002];
volatile unsigned movie_vlc_ds_done,movie_vlc_ds_failures;
int main(void) {
    consoleDemoInit();
    printf("FF9 VLC v2 on ARM9\nMode: %s\n",isDSiMode() ? "DSi" : "DS (67 MHz)");
    movie_vlc_tests();
    for (unsigned i=0;i<movie_vlc_fixture_count;i++) {
        Ff9MovieVlcInfo info;
        int status=ff9_movie_vlc_v2(movie_vlc_fixtures[i].bs,movie_vlc_fixtures[i].bytes,
                                  movie_vlc_ac_table,output,sizeof output/sizeof *output,&info);
        int pass=!status && info.blocks==movie_vlc_fixtures[i].blocks &&
                 info.halfwords*2==movie_vlc_fixtures[i].reference_bytes &&
                 !memcmp(output,movie_vlc_fixtures[i].reference,movie_vlc_fixtures[i].reference_bytes);
        printf("PSX fixture %u: %s\n",i,pass ? "PASS" : "FAIL");
        movie_vlc_ds_failures+=!pass;
    }
    movie_vlc_ds_done=1;
    printf("\n%s\n",movie_vlc_ds_failures ? "FAILED" : "ALL PASSED");
    while (1) swiWaitForVBlank();
}
