/* The bottom-screen log: libctru's console on the sub screen while the bring-up renderer only uses the top one. */
#include <3ds.h>
#include <stdio.h>

#include "ctr.h"

static PrintConsole console;
static int ready;

void ctr_console_init(void) {
    consoleInit(GFX_BOTTOM, &console);
    ready = 1;
}

/* Each line also goes to svcOutputDebugString: Azahar writes it to its log (Debug.Emulated), where tools/azahar.sh
 * collects it. */
void ctr_console_vlog(const char *fmt, va_list ap) {
    static char line[512];
    static int len;
    va_list copy;
    va_copy(copy, ap);
    if (ready) vprintf(fmt, ap);
    int n = vsnprintf(line + len, sizeof line - len, fmt, copy);
    va_end(copy);
    if (n > 0) len += n < (int)sizeof line - len ? n : (int)sizeof line - 1 - len;
    if (len && (line[len - 1] == '\n' || len >= (int)sizeof line - 1)) {
        svcOutputDebugString(line, len);
        len = 0;
    }
}

void ctr_console_show(void) {
    if (ready) consoleSelect(&console);
}
