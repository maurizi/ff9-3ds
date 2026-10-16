#include <nds.h>
#include <stdio.h>
#include <string.h>
#include "mdec_decode.h"
int mdec_tests(void);
extern const uint8_t mdec_fixture_tables[];
extern const unsigned mdec_fixture_count;
extern const struct {
    uint32_t command,halfwords;
    const uint16_t *rl;
    const uint8_t *pixels;
    unsigned bytes,consumed;
} mdec_fixtures[];
static uint8_t pixels[768];
volatile unsigned mdec_ds_done,mdec_ds_failures;
int main(void) {
    consoleDemoInit();
    printf("MDEC pixels on ARM9\nMode: %s\n",isDSiMode() ? "DSi" : "DS (67 MHz)");
    mdec_tests();
    PsxMdecEnv env;psx_mdec_env_init(&env,mdec_fixture_tables,mdec_fixture_tables+128);
    for (unsigned i=0;i<mdec_fixture_count;i++) {
        size_t consumed;
        int status=psx_mdec_macroblock(&env,mdec_fixtures[i].rl,mdec_fixtures[i].halfwords,
                                      mdec_fixtures[i].command,pixels,sizeof pixels,&consumed);
        int pass=!status && consumed==mdec_fixtures[i].consumed &&
                 !memcmp(pixels,mdec_fixtures[i].pixels,mdec_fixtures[i].bytes);
        printf("PSX fixture %u: %s\n",i,pass ? "PASS" : "FAIL");
        mdec_ds_failures+=!pass;
    }
    mdec_ds_done=1;
    printf("\n%s\n",mdec_ds_failures ? "FAILED" : "ALL PASSED");
    while (1) swiWaitForVBlank();
}
