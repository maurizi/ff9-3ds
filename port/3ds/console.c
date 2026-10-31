/* The log: every line to svcOutputDebugString (Azahar writes it to its log; tools/azahar.sh collects it), and the
 * last lines kept for the bottom screen, which shows them only after a fatal stop (the renderer owns it before). */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "ctr.h"

#define KEEP 28
static char lines[KEEP][64];
static int next_line;
static PrintConsole console;
static int shown;

void ctr_console_init(void) {}

void ctr_console_vlog(const char *fmt, va_list ap) {
    static char line[512];
    static int len;
    if (shown) {
        va_list copy;
        va_copy(copy, ap);
        vprintf(fmt, copy);
        va_end(copy);
    }
    int n = vsnprintf(line + len, sizeof line - len, fmt, ap);
    if (n > 0) len += n < (int)sizeof line - len ? n : (int)sizeof line - 1 - len;
    if (len && (line[len - 1] == '\n' || len >= (int)sizeof line - 1)) {
        svcOutputDebugString(line, len);
        snprintf(lines[next_line], sizeof lines[0], "%.*s", len, line);
        next_line = (next_line + 1) % KEEP;
        len = 0;
    }
}

void ctr_console_show(void) {
    if (shown) return;
    consoleInit(GFX_BOTTOM, &console);
    shown = 1;
    for (int i = 0; i < KEEP; i++) {
        const char *l = lines[(next_line + i) % KEEP];
        if (*l) printf("%s%s", l, l[strlen(l) - 1] == '\n' ? "" : "\n");
    }
}
