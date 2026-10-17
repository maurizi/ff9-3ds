/* PS1 BIOS functions whose results the game depends on, where the C library's version differs.
 * (memcpy, memset, strcmp, sprintf, ... come from libc: same results.)
 */
#include "psx_runtime.h"
#include "psx_mem.h"

/* A(2Fh) rand / A(30h) srand (psx-spx "BIOS Memory/String Functions").
 * The seed's power-on value isn't documented; it only matters for lockstep comparisons, which seed both sides. */
static unsigned int rand_seed = 0;

int rand(void) {
    rand_seed = rand_seed * 0x41C64E6D + 0x3039;
    return (rand_seed >> 16) & 0x7FFF;
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

/* A(19h) strcpy: the decomp's strcpy (port_prelude.h maps it here) */
char *psx_bios_strcpy(char *dst, const char *src) {
    if (!dst || !src) return 0;
    char *original = dst;
    dst = PSX_PTR(dst); src = PSX_PTR(src);
    char *d = dst;
    while ((*d++ = *src++)) {
    }
    return original;
}

/* A(15h) strcat (80063108): BIOS strings end at zero, including when their
 * payload contains FF9 high-byte glyph/control values. NULL inputs refuse
 * the copy; nonzero KUSEG pointers address the PSX RAM image. */
char *psx_bios_strcat(char *dst, const char *src) {
    if (!dst || !src) return 0;
    char *original = dst;
    dst = PSX_PTR(dst); src = PSX_PTR(src);
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
    return original;
}
