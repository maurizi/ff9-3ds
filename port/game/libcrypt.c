/* PAL copy protection (LibCrypt). On the PS1, file_initImg retries file_checkLibCrypt until it returns 0: the check
 * reads FF9.IMG's sector 0x13 in raw mode, unscrambles it with a key LibCrypt derives from the disc's subchannel, and
 * compares a CRC-16. The port reads the disc's files from its own pack, which has no subchannel, and the check is
 * hand-written MIPS the decomp keeps in asm: the disc counts as an original, the check's result for one. */
#define M2CTX
#include "ff9.h"

#if VERSION_PAL
s32 file_checkLibCrypt(void) {
    return 0;
}
#endif
