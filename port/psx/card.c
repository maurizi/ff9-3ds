/* Memory cards (libcard / BIOS _card_*, the BIOS card file system): two 128 KB cards, each a raw image in the
 * standard format (<data>/cards/card1.mcr, card2.mcr: what emulators and PS1 card dumps use, so saves move both
 * ways). psx-spx "Memory Card Data Format":
 *   block 0: frame 0 "MC" header; frames 1-15 the directory, one 128-byte entry per data block (state 0x51 first
 *   block of a file, 0x52 middle, 0x53 last, 0xA0 free, 0xA1-0xA3 deleted; file size in the first; next block or
 *   0xFFFF; the 20-byte name; an XOR checksum in byte 127); frames 16-35 the broken-sector list; frame 63 "MC".
 *   blocks 1-15: 8 KB of file data each.
 * The game (ovl_39) opens files with FASYNC and reads and writes 128-byte sectors, each completion a SwCARD event
 * (EvSpIOE) it polls with TestEvent; _card_info/_card_load answer on SwCARD, _card_clear on HwCARD. Requests complete
 * at once. A card is loaded on first use, created formatted if its file doesn't exist, and written back when a file
 * is closed or the directory changes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "psx_mem.h"
#include "psx_runtime.h"
typedef int s32; /* (not psyq_port.h: its PS1 string declarations clash with <string.h>) */

#define SW_CARD 0xF4000001
#define HW_CARD 0xF0000011
#define EV_SP_IOE 0x0004
#define EV_SP_ERROR 0x8000

#define CARD_SIZE 0x20000
#define BLOCK 0x2000
#define FRAME 0x80

void DeliverEvent(unsigned long desc, long spec);

static unsigned char cards[2][CARD_SIZE];
static int loaded[2], dirty[2];

static unsigned char *frame(int slot, int n) { return cards[slot] + n * FRAME; }

static void checksum(unsigned char *f) {
    unsigned char x = 0;
    for (int i = 0; i < FRAME - 1; i++) x ^= f[i];
    f[FRAME - 1] = x;
}

static void format(int slot) {
    unsigned char *c = cards[slot];
    memset(c, 0, CARD_SIZE);
    c[0] = 'M';
    c[1] = 'C';
    checksum(frame(slot, 0));
    for (int i = 1; i < 16; i++) {
        unsigned char *f = frame(slot, i);
        f[0] = 0xA0;
        f[8] = f[9] = 0xFF;
        checksum(f);
    }
    for (int i = 16; i < 36; i++) {
        unsigned char *f = frame(slot, i);
        memset(f, 0xFF, 4);
        f[8] = f[9] = 0xFF;
        checksum(f);
    }
    memcpy(frame(slot, 63), frame(slot, 0), FRAME);
    dirty[slot] = 1;
}

/* where saves go: the data folder (the PC host build's is read-only: build/) */
__attribute__((weak)) const char *plat_save_dir(void) { return plat_data_dir(); }
static void card_path(char *out, size_t n, int slot) {
    snprintf(out, n, "%s/cards/card%d.mcr", plat_save_dir(), slot + 1);
}

static void load(int slot) {
    if (loaded[slot]) return;
    loaded[slot] = 1;
    char path[256];
    card_path(path, sizeof path, slot);
    FILE *f = fopen(path, "rb");
    if (f && fread(cards[slot], 1, CARD_SIZE, f) == CARD_SIZE && cards[slot][0] == 'M' && cards[slot][1] == 'C') {
        fclose(f);
        return;
    }
    if (f) fclose(f);
    format(slot); /* (a new card: written out with the first save) */
    dirty[slot] = 0;
}

void plat_mkdir(const char *path); /* (platform: may not exist) */
__attribute__((weak)) void plat_mkdir(const char *path) { (void)path; }

/* the card written to its file; 0 if that failed (the card stays in memory, unsaved) */
static int flush(int slot) {
    if (!dirty[slot]) return 1;
    char path[256];
    snprintf(path, sizeof path, "%s/cards", plat_save_dir());
    plat_mkdir(path);
    card_path(path, sizeof path, slot);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(cards[slot], 1, CARD_SIZE, f) == CARD_SIZE;
    if (f && fclose(f) != 0) ok = 0;
    if (!ok) port_log("card: can't write %s\n", path);
    else dirty[slot] = 0;
    return ok;
}

/* "bu00:NAME" / "bu10:NAME" -> slot (0/1) and NAME; -1 if not a card path */
static int parse(const char *path, const char **name) {
    if (strncmp(path, "bu", 2) || path[4] != ':' || path[3] != '0') return -1;
    int slot = path[2] == '0' ? 0 : path[2] == '1' ? 1 : -1;
    if (slot >= 0) {
        load(slot);
        *name = path + 5;
    }
    return slot;
}

static int dir_state(int slot, int b) { return frame(slot, b)[0]; }
static unsigned dir_next(int slot, int b) { return frame(slot, b)[8] | frame(slot, b)[9] << 8; }
static unsigned dir_size(int slot, int b) {
    const unsigned char *f = frame(slot, b);
    return f[4] | f[5] << 8 | f[6] << 16 | (unsigned)f[7] << 24;
}

/* the directory entry (1-15) of the file called name, 0 if none */
static int find(int slot, const char *name) {
    for (int b = 1; b < 16; b++)
        if (dir_state(slot, b) == 0x51 && !strncmp((const char *)frame(slot, b) + 10, name, 20)) return b;
    return 0;
}

/* ---- libcard ---------------------------------------------------------------------------------------------------- */

void InitCARD(long val) { (void)val; }
long StartCARD(void) { return 1; }
long StopCARD(void) { return 1; }
void _bu_init(void) {}

long _card_info(long chan) {
    int slot = (chan >> 4) & 1;
    load(slot);
    DeliverEvent(SW_CARD, EV_SP_IOE);
    return 1;
}

long _card_load(long chan) {
    load((chan >> 4) & 1);
    DeliverEvent(SW_CARD, EV_SP_IOE);
    return 1;
}

long _card_clear(long chan) {
    (void)chan;
    DeliverEvent(HW_CARD, EV_SP_IOE);
    return 1;
}

long _card_wait(long chan) {
    (void)chan;
    return 1;
}

long _card_format(long chan) {
    int slot = (chan >> 4) & 1;
    loaded[slot] = 1;
    format(slot);
    return flush(slot); /* (0: not written, as a failed format) */
}

/* ---- the BIOS card file system: open, lseek, read, write, close, firstfile, nextfile, erase ---------------------- */

#define FREAD 0x0001
#define FWRITE 0x0002
#define FASYNC 0x8000
#define FCREAT 0x0200

static struct { int used, slot, head; unsigned pos, size; long mode; } fds[4];

long psx_bios_open(const char *name, long mode) {
    name = PSX_PTR(name);
    const char *file;
    int slot = parse(name, &file);
    if (slot < 0) return -1;
    int head = find(slot, file);
    if (mode & FCREAT) {
        int blocks = (int)((mode >> 16) & 0xFFFF);
        if (head || blocks < 1 || blocks > 15) return -1;
        int chain[15], n = 0;
        for (int b = 1; b < 16 && n < blocks; b++)
            if ((dir_state(slot, b) & 0xF0) == 0xA0) chain[n++] = b;
        if (n < blocks) return -1; /* card full */
        for (int i = 0; i < n; i++) {
            unsigned char *f = frame(slot, chain[i]);
            memset(f, 0, FRAME);
            f[0] = i == 0 ? 0x51 : i == n - 1 ? 0x53 : 0x52;
            if (i == 0) {
                unsigned size = (unsigned)n * BLOCK;
                f[4] = size; f[5] = size >> 8; f[6] = size >> 16; f[7] = size >> 24;
                strncpy((char *)f + 10, file, 20);
            }
            unsigned next = i + 1 < n ? (unsigned)chain[i + 1] - 1 : 0xFFFF;
            f[8] = next; f[9] = next >> 8;
            checksum(f);
            memset(cards[slot] + chain[i] * BLOCK, 0, BLOCK);
        }
        dirty[slot] = 1;
        head = chain[0];
    } else if (!head) {
        return -1;
    }
    for (int fd = 0; fd < 4; fd++)
        if (!fds[fd].used) {
            fds[fd].used = 1;
            fds[fd].slot = slot;
            fds[fd].head = head;
            fds[fd].pos = 0;
            fds[fd].size = dir_size(slot, head);
            fds[fd].mode = mode;
            return fd + 2; /* (0 and 1 are the console's) */
        }
    return -1;
}

static int fd_ok(s32 fd) { return fd >= 2 && fd < 6 && fds[fd - 2].used; }

s32 psx_bios_lseek(s32 fd, s32 offset, s32 whence) {
    if (!fd_ok(fd)) return -1;
    unsigned pos = whence == 0 ? (unsigned)offset : fds[fd - 2].pos + offset;
    if (pos > fds[fd - 2].size) return -1;
    fds[fd - 2].pos = pos;
    return (s32)pos;
}

/* the card byte of the file's byte `pos` (its blocks are a chain from its head) */
static unsigned char *file_byte(int slot, int head, unsigned pos) {
    int b = head;
    for (unsigned k = pos / BLOCK; k; k--) {
        unsigned next = dir_next(slot, b);
        if (next == 0xFFFF) return NULL;
        b = (int)next + 1;
    }
    return cards[slot] + b * BLOCK + pos % BLOCK;
}

static s32 transfer(s32 fd, s32 buf, s32 n, int writing) {
    if (!fd_ok(fd) || n < 0) {
        DeliverEvent(SW_CARD, EV_SP_ERROR);
        return -1;
    }
    unsigned char *p = PSX_PTR((uintptr_t)buf);
    int k = fd - 2, slot = fds[k].slot;
    unsigned start = fds[k].pos;
    if (fds[k].pos + (unsigned)n > fds[k].size) n = (s32)(fds[k].size - fds[k].pos);
    for (s32 done = 0; done < n;) { /* (block by block: a file's blocks needn't be adjacent) */
        unsigned char *c = file_byte(slot, fds[k].head, fds[k].pos);
        if (!c) break;
        s32 run = BLOCK - (s32)(fds[k].pos % BLOCK);
        if (run > n - done) run = n - done;
        if (writing) memcpy(c, p + done, (size_t)run);
        else memcpy(p + done, c, (size_t)run);
        fds[k].pos += (unsigned)run;
        done += run;
    }
    if (writing) dirty[slot] = 1;
    /* a file's writing begun (from its start, as a save writes it): the card written out now, and a failure reported
     * as the card's error (the save screen's "failed" path, ovl_39 menuSave_cardPoll). Left to the close, whose result
     * nothing checks, the save screen said "Data saved." when the SD card couldn't be written. (The save writes 40 of
     * its file's 64 sectors: not to its end) */
    int failed = writing && start == 0 && !flush(slot);
    if (failed) loaded[slot] = 0, load(slot); /* (the card as it is on the SD card: the save list shows what was saved) */
    if (fds[k].mode & FASYNC) {
        /* the BIOS's asynchronous card read leaves the file position where it was (the game seeks past each sector
         * it read: ovl_39 menuSave_cardPoll); a write moves it */
        if (!writing) fds[k].pos -= (unsigned)n;
        DeliverEvent(SW_CARD, failed ? EV_SP_ERROR : EV_SP_IOE);
    }
    if (failed && !(fds[k].mode & FASYNC)) return -1;
    return n;
}

s32 psx_bios_read(s32 fd, s32 buf, s32 n) { return transfer(fd, buf, n, 0); }
s32 psx_bios_write(s32 fd, s32 buf, s32 n) { return transfer(fd, buf, n, 1); }

s32 psx_bios_close(s32 fd) {
    if (!fd_ok(fd)) return -1;
    fds[fd - 2].used = 0;
    flush(fds[fd - 2].slot);
    return fd;
}

/* libapi struct DIRENTRY */
typedef struct DirEntry {
    char name[20];
    s32 attr, size;
    struct DirEntry *next;
    s32 head;
    char system[4];
} DirEntry;

static int iter_slot, iter_block;

static void *fill_next(DirEntry *d) {
    for (; iter_block < 16; iter_block++)
        if (dir_state(iter_slot, iter_block) == 0x51) {
            const unsigned char *f = frame(iter_slot, iter_block);
            memset(d, 0, sizeof *d);
            memcpy(d->name, f + 10, 20);
            d->attr = 0x51;
            d->size = (s32)dir_size(iter_slot, iter_block);
            d->head = iter_block;
            iter_block++;
            return d;
        }
    return NULL;
}

void *firstfile(char *name, void *dir) {
    const char *file;
    int slot = parse(PSX_PTR(name), &file);
    if (slot < 0) return NULL;
    iter_slot = slot;
    iter_block = 1;
    return fill_next(PSX_PTR(dir));
}

s32 psx_bios_nextfile(s32 dir) {
    DirEntry *d = PSX_PTR((uintptr_t)dir);
    return fill_next(d) ? dir : 0;
}

/* B(45h) erase: delete a file (its blocks' states 0xA1-0xA3, as the BIOS leaves them) */
long psx_bios_erase(const char *name) {
    const char *file;
    int slot = parse(PSX_PTR(name), &file);
    int b = slot < 0 ? 0 : find(slot, file);
    if (!b) return 0;
    for (int n = 0; n < 15 && b; n++) {
        unsigned char *f = frame(slot, b);
        f[0] = (unsigned char)(f[0] - 0x51 + 0xA1);
        checksum(f);
        unsigned next = dir_next(slot, b);
        b = next == 0xFFFF ? 0 : (int)next + 1;
    }
    dirty[slot] = 1;
    return flush(slot); /* (0: the erase not written) */
}
