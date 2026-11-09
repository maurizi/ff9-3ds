/* psxrel.bin: which words of the game's data are pointers and what each becomes (tools/gen_relocs.py, made from the
 * user's discs by the builder or by make). The program carries none of the game's bytes: main's data is read from
 * the disc's executable at boot and an overlay's when the game loads it, then these tables convert their pointer
 * words; code pointers are positions in psx_native_index (tools/gen_link.py). The file sits beside the disc pack
 * (the full builds' romfs, or the data folder) and must have been made for this link (psx_linkinfo_id). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psx_mem.h"
#include "platform.h"
#include "psx_runtime.h"

extern const unsigned int psx_native_index[], psx_native_count, psx_linkinfo_id;
const char *plat_disc_pack(void); /* port/psx/cd.c (weak) or the platform's */
/* the platform's last place to look (the PC: build/psxrel.bin, where make writes it); NULL: none */
__attribute__((weak)) const char *plat_psxrel_fallback(void) { return NULL; }

static unsigned int *rel, rel_sections;

/* a data word that points to a function the port doesn't have (its index entry is 0) points here instead */
static void missing_native(void) {
    port_fatal("the game called through a data pointer to a function this build doesn't have");
}

static int load(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    const long size = lseek(fd, 0, SEEK_END);
    unsigned int *buf = size >= 16 ? malloc((size_t)size) : NULL;
    long got = 0;
    if (buf && lseek(fd, 0, SEEK_SET) == 0)
        while (got < size) {
            const int n = (int)read(fd, (char *)buf + got, (size_t)(size - got));
            if (n <= 0) break;
            got += n;
        }
    close(fd);
    if (!buf || got != size) port_fatal("%s: can't read it", path);
    if (memcmp(buf, "FF9R", 4) != 0 || buf[1] != 1) port_fatal("%s: not a version 1 relocation table", path);
    if (buf[2] != psx_linkinfo_id)
        port_fatal("%s was made for another build of the program: make it again with this one's builder (or make)",
                   path);
    rel = buf;
    rel_sections = buf[3];
    port_log("relocations: %s\n", path);
    return 1;
}

void psx_relocs_load(void) {
    if (rel) return;
    char path[512];
    const char *pack = plat_disc_pack();
    if (pack) { /* beside the disc pack */
        const char *slash = strrchr(pack, '/');
        snprintf(path, sizeof path, "%.*s/psxrel.bin", slash ? (int)(slash - pack) : 0, pack);
        if (load(path)) return;
    }
    snprintf(path, sizeof path, "%s/psxrel.bin", plat_data_dir());
    if (load(path)) return;
    const char *fallback = plat_psxrel_fallback();
    if (fallback && load(fallback)) return;
    port_fatal("no psxrel.bin beside the disc pack or in %s: build the game with the builder (or make)",
               plat_data_dir());
}

const unsigned int *psx_relocs(unsigned int section) {
    for (unsigned int i = 0; i < rel_sections; i++)
        if (rel[4 + 3 * i] == section) return rel + rel[4 + 3 * i + 1] / 4;
    return NULL;
}

void psx_relocate(const unsigned int *r, unsigned int off, unsigned int len) {
    for (; *r != 0xFFFFFFFFu; r += 1 + (*r >> 29 >= 2) + (*r >> 29 == 3)) {
        const unsigned int at = *r & 0x1FFFFF, kind = *r >> 29;
        if (at < off) continue;
        if (at >= off + len) break;
        unsigned int *word = (unsigned int *)(psx_ram + at);
        if (kind == 0 && (*word >> 24) == 0x80) *word = (unsigned int)(psx_ram + (*word & 0x1FFFFF));
        else if (kind == 1 && (*word >> 16) == 0x1F80) *word = (unsigned int)(psx_scratchpad + (*word & 0x3FF));
        else if (kind >= 2) {
            const unsigned int fn = r[1] < psx_native_count ? psx_native_index[r[1]] : 0;
            *word = fn ? fn + (kind == 3 ? r[2] : 0) : (unsigned int)missing_native;
        }
    }
}
