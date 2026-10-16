#include <nds.h>
#include <stdio.h>
extern int spu_adpcm_tests(void);
extern int spu_source_tests(void);
extern int spu_envelope_tests(void);
extern int spu_interpolation_tests(void);
extern int spu_voice_tests(void);
int main(void) {
    consoleDemoInit();
    printf("SPU components on ARM9\n\n");
    int result = spu_adpcm_tests();
    result |= spu_source_tests();
    printf("Envelope reference running...\n");
    result |= spu_envelope_tests();
    result |= spu_interpolation_tests();
    result |= spu_voice_tests();
    if (result == 0) printf("All SPU component checks PASS\n");
    else printf("SPU component checks FAILED\n");
    while (1) swiWaitForVBlank();
}
