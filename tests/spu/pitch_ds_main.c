#include <nds.h>
#include <stdio.h>
extern int akao_pitch_abi_tests(void);
int main(void) {
    consoleDemoInit();puts("AKAO pitch calls on ARM9\n");
    int result=akao_pitch_abi_tests();
    puts(result?"Pitch ABI FAILED":"Pitch ABI checks PASS");
    while(1)swiWaitForVBlank();
}
