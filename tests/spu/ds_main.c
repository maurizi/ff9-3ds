#include <nds.h>
#include <stdio.h>
extern int spu_adpcm_tests(void);
int main(void) {
    consoleDemoInit();
    printf("SPU decoder on ARM9\n\n");
    if (spu_adpcm_tests() == 0) printf("SPU decoder checks PASS\n");
    else printf("SPU decoder checks FAILED\n");
    while (1) swiWaitForVBlank();
}
