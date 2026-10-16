#include <nds.h>
#include <stdio.h>
extern int spu_core_tests(void);
extern int spu_noise_tests(void);
int main(void) {
    consoleDemoInit();
    puts("SPU integration on ARM9\n");
    int result = spu_noise_tests();
    result |= spu_core_tests();
    puts(result ? "SPU integration FAILED" : "SPU integration checks PASS");
    while (1) swiWaitForVBlank();
}
