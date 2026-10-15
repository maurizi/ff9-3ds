/* Reuse the recording GPU backend, without executing its ordering tests. */
#include <stdio.h>
#define main ordering_test_main
#include "ordering_test.c"
#undef main

int main(void) {
    unsigned int immediate;
    DRAWENV env;
    while (fread(&immediate, sizeof immediate, 1, stdin) == 1) {
        assert(fread(&env, sizeof env, 1, stdin) == 1);
        count = 0;
        if (immediate) PutDrawEnv(&env);
        else SetDrawEnv(&env.dr_env, &env);
        assert(fwrite(&env.dr_env, sizeof env.dr_env, 1, stdout) == 1);
    }
    return 0;
}
