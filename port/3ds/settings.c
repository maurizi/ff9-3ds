/* The port's options (the field screen's OPTIONS page, port/game/field_hud.c), kept in sdmc:/3ds/ff9/settings.cfg as
 * "name=value" lines: read at start, written on each change. */
#include <stdio.h>
#include <string.h>

#include "platform.h"

enum { SET_DEPTH, SET_SHARP, SET_VOICES, SET_COUNT };
static const char *const names[SET_COUNT] = {"depth", "sharp", "voices"};
static const int defaults[SET_COUNT] = {1, 0, 1}; /* depth: 0 50%, 1 100%, 2 150% */
static int values[SET_COUNT], loaded;

static void path(char *p, unsigned n) { snprintf(p, n, "%s/settings.cfg", plat_data_dir()); }

static void load(void) {
    loaded = 1;
    memcpy(values, defaults, sizeof values);
    char p[256], line[64];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        for (int k = 0; k < SET_COUNT; k++) {
            size_t n = strlen(names[k]);
            if (!strncmp(line, names[k], n) && line[n] == '=') sscanf(line + n + 1, "%d", &values[k]);
        }
    fclose(f);
}

int port_setting_get(int k) {
    if (!loaded) load();
    return k >= 0 && k < SET_COUNT ? values[k] : 0;
}

void port_setting_set(int k, int v) {
    if (!loaded) load();
    if (k < 0 || k >= SET_COUNT || values[k] == v) return;
    values[k] = v;
    char p[256];
    path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < SET_COUNT; i++) fprintf(f, "%s=%d\n", names[i], values[i]);
    fclose(f);
}
