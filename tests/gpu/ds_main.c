/* Run the production GPU transfer/ordering checks on ARM9, without game data. */
#include <nds.h>
#include <stdio.h>

extern int gpu_transfer_tests(void);
int main(void) {
    consoleDemoInit();
    printf("GPU transfers on ARM9\n\n");
    if(gpu_transfer_tests()!=0) {
        printf("GPU transfer checks FAILED\n");
    } else {
        printf("147 overlap references PASS\n");
        printf("Masks, padding, wrapping,\n");
        printf("and packet order PASS\n");
    }
    while(1) swiWaitForVBlank();
}
