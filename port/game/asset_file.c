/* A file of the port's own beside the disc data (the baked maps: tools/bake_maps.py), read with stdio here, apart
 * from the game's headers (their printf and friends clash with stdio's). */
#include <stdio.h>

/* the first of name's places that opens: the app's romfs, the full 3dsx's (mounted "full:"), the SD card's game
 * folder; on the host, build/ */
void *port_asset_open(const char *name) {
#ifdef PORT_3DS
    static const char *const dirs[] = {"romfs:/", "full:/", "sdmc:/3ds/ff9/"};
#else
    static const char *const dirs[] = {"build/"};
#endif
    char path[96];
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        FILE *f = fopen(path, "rb");
        if (f) return f;
    }
    return NULL;
}
int port_asset_read(void *f, void *buf, unsigned long n) { return fread(buf, 1, n, (FILE *)f) == n; }
int port_asset_seek(void *f, unsigned long at) { return fseek((FILE *)f, (long)at, SEEK_SET) == 0; }
void port_asset_close(void *f) { fclose((FILE *)f); }
