/* libcd: a virtual CD drive that reads the user's raw disc image (2352-byte sectors, as the decomp's disks/).
 *
 * The game's CD module (main: cd_startRequest, cd_onSectorReady, g_cdLoad ...) runs on top with loader-poll hooks. It is
 * an interrupt-driven state machine: it issues a command, and the drive's interrupt calls back the sync callback
 * (command complete) or the ready callback (a sector is ready, fetched with CdGetSector). It also busy-waits on
 * its own state with no library call inside the loop. Reviewed loader polling
 * sites therefore service the drive, as do frame boundaries and libcd calls.
 * Each pump completes queued commands but delivers at most one sector. Draining
 * a whole read immediately can overwrite a released summon model while its
 * final callbacks still query it (Ifrit). The frame pump (psx_cd_frame) instead
 * delivers what the drive reads in a 60 Hz frame (75 or 150 sectors/s): streamed
 * movies read without polling and need the real rate. Commands issued inside a callback
 * are queued and processed after it returns, without recursive sector delivery.
 * This preserves asynchronous progress; it does not model physical seek time.
 *
 * Disc layout: Mode 2 Form 1, user data at offset 24 of each raw sector. Disc swaps: psx_cd_swap (all four images
 * are on the card: the disc-change screen's check finds the disc it asked for).
 */
#include "blackbox.h"
#include "psyq_port.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "platform.h"
#include "profile.h"

#define RAW_SECTOR 2352
#define USER_OFFSET 24
#define MAX_SECTORS_PER_PUMP 1 /* keep data delivery separate from command completion */

/* libcd command numbers (libcd.h CdlXxx; the decomp's psyq.h defines some) */
#ifndef CdlNop
#define CdlNop 0x01
#endif
#ifndef CdlSetloc
#define CdlSetloc 0x02
#endif
#ifndef CdlPlay
#define CdlPlay 0x03
#endif
#ifndef CdlForward
#define CdlForward 0x04
#endif
#ifndef CdlBackward
#define CdlBackward 0x05
#endif
#ifndef CdlReadN
#define CdlReadN 0x06
#endif
#ifndef CdlStandby
#define CdlStandby 0x07
#endif
#ifndef CdlStop
#define CdlStop 0x08
#endif
#ifndef CdlPause
#define CdlPause 0x09
#endif
#ifndef CdlInit
#define CdlInit 0x0A
#endif
#ifndef CdlMute
#define CdlMute 0x0B
#endif
#ifndef CdlDemute
#define CdlDemute 0x0C
#endif
#ifndef CdlSetfilter
#define CdlSetfilter 0x0D
#endif
#ifndef CdlSetmode
#define CdlSetmode 0x0E
#endif
#ifndef CdlGetparam
#define CdlGetparam 0x0F
#endif
#ifndef CdlGetlocL
#define CdlGetlocL 0x10
#endif
#ifndef CdlGetlocP
#define CdlGetlocP 0x11
#endif
#ifndef CdlGetTN
#define CdlGetTN 0x13
#endif
#ifndef CdlGetTD
#define CdlGetTD 0x14
#endif
#ifndef CdlSeekL
#define CdlSeekL 0x15
#endif
#ifndef CdlSeekP
#define CdlSeekP 0x16
#endif
#ifndef CdlReadS
#define CdlReadS 0x1B
#endif
/* interrupt kinds passed to the callbacks */
#ifndef CdlNoIntr
#define CdlNoIntr 0
#endif
#ifndef CdlDataReady
#define CdlDataReady 1
#endif
#ifndef CdlComplete
#define CdlComplete 2
#endif
#ifndef CdlAcknowledge
#define CdlAcknowledge 3
#endif
#ifndef CdlDataEnd
#define CdlDataEnd 4
#endif
#ifndef CdlDiskError
#define CdlDiskError 5
#endif
/* status bits */
#define STAT_MOTOR 0x02
#define STAT_READ 0x20
#define STAT_SEEK 0x40

typedef void (*CdCallback)(u8 intr, u8 *result);

/* the game's names for libcd's callback setters are still placeholders in the decomp */
static CdCallback sync_cb, ready_cb, read_cb;

/* (POSIX I/O, not stdio: on the DS a FILE costs the heap ~600 bytes more for each file open, which battles are short of) */
static int disc = -1;
/* n bytes at `at` (0: short or failed) */
static int read_at(int fd, long at, void *buf, unsigned int n) {
    if (at >= 0 && lseek(fd, at, SEEK_SET) != at) return 0;
    for (u8 *q = buf; n;) {
        const int got = (int)read(fd, q, n);
        if (got <= 0) return 0;
        q += got; n -= (unsigned int)got;
    }
    return 1;
}
static int disc_no = 1;

int psx_cd_disc(void) {
    return disc_no;
}

void psx_cd_select_disc(unsigned selected) {
    if(selected<1 || selected>4) port_fatal("invalid disc %u",selected);
    if(disc >= 0) port_fatal("disc selection must precede game initialization");
    disc_no=(int)selected;
}

/* libcd's own globals, which game C reads directly (CdStatus CdStatus, CdMode CdMode, CdLastPos):
 * kept in PS1 RAM at their original addresses so those reads see the drive's state. */
extern u8 D_8006825C;      /* status byte */
extern s32 D_8006826C;     /* mode */
extern u8 D_80068268[4];   /* last position (CdlLOC) */
#define status D_8006825C
#define mode D_8006826C
#define last_pos D_80068268
static unsigned int loc, read_lba; /* Setloc target, next sector to deliver */
static int reading;
static u8 sector[RAW_SECTOR] __attribute__((aligned(32)));
static unsigned int sector_lba; /* LBA of `sector` */
static unsigned int sector_pos; /* bytes of `sector`'s user data already taken by CdGetSector */
#ifdef PORT_DISC_PACK
static int sector_rebuilt;      /* `sector` came from a disc pack's data record: no EDC/ECC */
#endif

#define QUEUE 16
static struct {
    u8 com;
    u8 param[4];
} queue[QUEUE];
static int q_head, q_tail, pumping;
/* debug snapshot for the platform's watchdog */
unsigned int psx_cd_debug[6];
unsigned int psx_cd_read_starts; /* (post-mortems: reads started, as by a stream's seeks) */
static int last_com_done;

/* ---- disc image ----------------------------------------------------------------------------------------- */

static long disc_at = -1; /* the image's position after the last read */

#ifdef PORT_DISC_PACK
/* <data>/discs.bin, when there (tools/pack_discs.py: the packed ROM's four discs in one), else raw/FF9 CD<N>.img.
 * The pack keeps each distinct sector once: game data as its 2048 bytes of user data (sync, address and data
 * subheader rebuilt here; EDC/ECC zero), the rest (the movies) whole. Runs map a disc's LBAs to those records. */
static int packed;
static unsigned int pack_table, pack_runs, pack_sectors, pack_data, pack_raw;
#define PACK_DATA 1u   /* run flags: 2048-byte data records (else 2352-byte raw ones) */
#define PACK_REPEAT 2u /* every sector of the run is the same record */
#define PACK_BLOCK 16
static struct { unsigned int lba, count, off, pad; } pack_buf[PACK_BLOCK]; /* a block of the table's runs */
static unsigned int pack_first, pack_n;
static int sector_is_data; /* the last read_raw rebuilt a data record (no EDC/ECC) */

static unsigned int le32(const u8 *p);
static void pack_read(long at, void *buf, unsigned int bytes) {
    if (!read_at(disc, at, buf, bytes))
        port_fatal("disc %d: can't read discs.bin at %ld", disc_no, at);
    disc_at = at + (long)bytes;
}
#endif

static int open_disc(void) {
    if (disc >= 0) {
        return 1;
    }
    char path[512];
#ifdef PORT_DISC_PACK
    snprintf(path, sizeof path, "%s/discs.bin", plat_data_dir());
    if ((disc = open(path, O_RDONLY)) >= 0) {
        u8 h[32];
        pack_read(0, h, 32);
        if (__builtin_memcmp(h, "FF9D", 4) != 0 || le32(h + 4) != 1 || le32(h + 8) < (unsigned int)disc_no)
            port_fatal("%s: not a version 1 disc pack with disc %d", path, disc_no);
        pack_data = le32(h + 12);
        pack_raw = le32(h + 16);
        pack_read(32 + 12 * (disc_no - 1), h, 12);
        pack_table = le32(h);
        pack_runs = le32(h + 4);
        pack_sectors = le32(h + 8);
        if (!pack_sectors) port_fatal("%s: no disc %d in this pack (make ds-packed PACK_DISCS=...)", path, disc_no);
        packed = 1;
        return 1;
    }
#endif
    snprintf(path, sizeof path, "%s/raw/FF9 CD%d.img", plat_data_dir(), disc_no);
    disc = open(path, O_RDONLY);
    if (disc < 0) {
        port_fatal("can't open disc image %s", path);
    }
    return 1;
}

static int n_code_files; /* (below: the code overlays' sectors, scanned per disc) */
/* Put disc `n` (1-4) in the drive: the image is reopened at its next read; what the drive buffered is dropped. */
void psx_cd_swap(unsigned n) {
    if (n < 1 || n > 4 || (int)n == disc_no) return;
    if (disc >= 0) close(disc);
    disc = -1;
    disc_no = (int)n;
    disc_at = -1;
    reading = 0;
    sector_lba = ~0u;
    sector_pos = 0;
#ifdef PORT_DISC_PACK
    packed = 0;
    pack_first = pack_n = 0;
#endif
    n_code_files = -1;
    port_log("cd: disc %u in the drive\n", n);
}

#ifdef PORT_DISC_PACK
/* The run holding `lba`: from the cached block of the table, else the block from it read: the next one when the
 * reads go on past this one's end, else found by a binary search of the table (at a seek). */
static unsigned int pack_run(unsigned int lba) {
    static unsigned int last;
    if (last < pack_n && lba - pack_buf[last].lba < pack_buf[last].count) return last;
    if (pack_n && lba >= pack_buf[0].lba)
        for (unsigned int i = 0; i < pack_n; i++)
            if (lba - pack_buf[i].lba < pack_buf[i].count) return last = i;
    if (lba >= pack_sectors) port_fatal("disc %d: sector %u past the disc's end", disc_no, lba);
    unsigned int lo = 0, hi = pack_runs; /* the last run starting at or before lba */
    if (pack_n && lba == pack_buf[pack_n - 1].lba + pack_buf[pack_n - 1].count) hi = (lo = pack_first + pack_n) + 1;
    while (hi - lo > 1) {
        unsigned int mid = (lo + hi) / 2;
        u8 e[4];
        pack_read((long)(pack_table + 16 * mid), e, 4);
        if (le32(e) <= lba) lo = mid; else hi = mid;
    }
    pack_first = lo;
    pack_n = pack_runs - lo < PACK_BLOCK ? pack_runs - lo : PACK_BLOCK;
    pack_read((long)(pack_table + 16 * lo), pack_buf, 16 * pack_n);
    for (unsigned int i = 0; i < pack_n; i++) { /* (count | flags << 28: the flags to pad) */
        pack_buf[i].pad = pack_buf[i].count >> 28;
        pack_buf[i].count &= 0x0FFFFFFFu;
    }
    return last = 0;
}
#endif

static void patch_load(void);
static void patch_apply(unsigned int lba, u8 *raw);
static int n_runs = -1, runs_disc;
unsigned int psx_cd_sectors_read, psx_cd_last_lba; /* (the 3DS's heartbeat) */
static void read_raw(unsigned int lba, u8 *buf) {
    unsigned int t = port_profile_begin();
    psx_cd_sectors_read++;
    psx_cd_last_lba = lba;
    open_disc();
    /* (before this sector's read: loading the table reads others through read_user's buffer, which may be buf) */
    if (n_runs < 0 || runs_disc != disc_no) patch_load();
    BB(0x43440001, lba); BB(0x43440002, buf);
    /* (no seek for the next sector: on the DS a seek behind the file system's read-ahead walks the image's cluster
     * chain from its start, which costs more the further into the disc) */
    long at = (long)lba * RAW_SECTOR;
    unsigned int bytes = RAW_SECTOR;
    u8 *dst = buf;
#ifdef PORT_DISC_PACK
    sector_is_data = 0;
    if (packed) {
        unsigned int r = pack_run(lba);
        unsigned int flags = pack_buf[r].pad;
        sector_is_data = flags & PACK_DATA;
        bytes = sector_is_data ? 2048 : RAW_SECTOR;
        at = (long)((sector_is_data ? pack_data : pack_raw) + pack_buf[r].off +
                    (flags & PACK_REPEAT ? 0 : (lba - pack_buf[r].lba) * bytes));
        if (sector_is_data) dst = buf + USER_OFFSET;
    }
#endif
    if (!read_at(disc, at != disc_at ? at : -1, dst, bytes)) {
        disc_at = -1;
        port_fatal("disc %d: can't read sector %u", disc_no, lba);
    }
    disc_at = at + (long)bytes;
#ifdef PORT_DISC_PACK
    if (packed) {
        if (sector_is_data) {
            static const u8 head[USER_OFFSET] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
                                                 0, 0, 0, 2, 0, 0, 8, 0, 0, 0, 8, 0};
            __builtin_memcpy(buf, head, USER_OFFSET);
            __builtin_memset(buf + USER_OFFSET + 2048, 0, RAW_SECTOR - USER_OFFSET - 2048);
        }
        unsigned int a = lba + 150;
        buf[12] = (u8)((a / 4500 / 10) << 4 | a / 4500 % 10);
        buf[13] = (u8)((a / 75 % 60 / 10) << 4 | a / 75 % 60 % 10);
        buf[14] = (u8)((a % 75 / 10) << 4 | a % 75 % 10);
    }
#endif
    patch_apply(lba, buf);
    port_profile_detail_end(PORT_PROFILE_CD_READ, t);
}

static void read_user(unsigned int lba, u8 *buf2048) {
    /* not on the stack: the DS's stack is in DTCM, which DMA can't reach, and a card driver may read by DMA (onto
     * main RAM at the same address: on a 4 MB DS, the top of the heap) */
    static u8 raw[RAW_SECTOR] __attribute__((aligned(32)));
    read_raw(lba, raw);
    __builtin_memcpy(buf2048, raw + USER_OFFSET, 2048);
}

/* n sectors' user data from lba on (the IMG's absolute sectors), now: for the port's own reads (the 3DS's bottom
 * screen reads the menu's portraits in a field), outside the game's CD queue */
void psx_cd_read_data(unsigned int lba, void *buf, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) read_user(lba + i, (u8 *)buf + i * 2048);
}

/* ---- sector patches ------------------------------------------------------------------------------------
 * Sectors of FF9.IMG served from <data>/patches/disc<N>.bin instead of the image (tools/simplify_battle_models.py:
 * the heaviest battle enemies' models, simplified at build time): "FF9P", a run count, then {IMG sector, sectors,
 * file offset} per run; each sector's 2048 bytes of user data. No file: none. */
#define PATCH_RUNS 32
static struct { unsigned int lba, count, off; } runs[PATCH_RUNS];
static int patch_file = -1;

static int iso_lookup(const char *name, unsigned int *lba, unsigned int *size);
static unsigned int le32(const u8 *p);
static void patch_load(void) {
    n_runs = 0; /* (iso_lookup below reads sectors: none patched meanwhile) */
    runs_disc = disc_no;
    if (patch_file >= 0) { close(patch_file); patch_file = -1; }
    char path[512];
    snprintf(path, sizeof path, "%s/patches/disc%d.bin", plat_data_dir(), disc_no);
    const int f = open(path, O_RDONLY);
    if (f < 0) return;
    u8 h[12];
    unsigned int base, size;
    if (!read_at(f, 0, h, 8) || __builtin_memcmp(h, "FF9P", 4) != 0 || le32(h + 4) > PATCH_RUNS ||
        !iso_lookup("\\FF9.IMG;1", &base, &size)) {
        close(f);
        return;
    }
    const int n = (int)le32(h + 4);
    for (int i = 0; i < n; i++) {
        if (!read_at(f, 8 + 12 * i, h, 12)) { close(f); return; }
        runs[i].lba = base + le32(h); runs[i].count = le32(h + 4); runs[i].off = le32(h + 8);
    }
    patch_file = f;
    n_runs = n;
}

static void patch_apply(unsigned int lba, u8 *raw) {
    for (int i = 0; i < n_runs; i++)
        if (lba - runs[i].lba < runs[i].count) {
            if (!read_at(patch_file, (long)(runs[i].off + (lba - runs[i].lba) * 2048), raw + USER_OFFSET, 2048))
                port_fatal("disc %d: can't read patched sector %u", disc_no, lba);
            return;
        }
}

/* ---- positions ------------------------------------------------------------------------------------------ */

static u8 bcd(int v) {
    return (u8)(((v / 10) << 4) | (v % 10));
}

static int unbcd(u8 v) {
    return (v >> 4) * 10 + (v & 0xF);
}

int CdPosToInt(CdlLOC *p) {
    return (unbcd(p->minute) * 60 + unbcd(p->second)) * 75 + unbcd(p->sector) - 150;
}

CdlLOC *CdIntToPos(int i, CdlLOC *p) {
    i += 150;
    p->sector = bcd(i % 75);
    p->second = bcd(i / 75 % 60);
    p->minute = bcd(i / 75 / 60);
    return p;
}

/* ---- ISO9660 lookup (CdSearchFile) ---------------------------------------------------------------------- */

static unsigned int le32(const u8 *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24;
}

/* Find `name` ("\\DIR\\FILE.EXT;1") on the disc: fills lba and size. */
static int iso_lookup(const char *name, unsigned int *lba, unsigned int *size) {
    u8 buf[2048];
    read_user(16, buf);
    if (buf[1] != 'C' || buf[2] != 'D') {
        return 0;
    }
    unsigned int dir = le32(buf + 156 + 2), dir_size = le32(buf + 156 + 10);
    const char *p = name;
    while (*p == '\\' || *p == '/') {
        p++;
    }
    while (*p) {
        char part[32];
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < 31) {
            part[n++] = *p++;
        }
        part[n] = 0;
        while (*p == '\\' || *p == '/') {
            p++;
        }
        int found = 0;
        for (unsigned int s = 0; s < (dir_size + 2047) / 2048 && !found; s++) {
            read_user(dir + s, buf);
            for (unsigned int i = 0; i < 2048 && buf[i] && !found;) {
                unsigned int len = buf[i];
                unsigned int nl = buf[i + 32];
                const char *fn = (const char *)buf + i + 33;
                int match = nl == (unsigned int)n || (nl > (unsigned int)n && fn[n] == ';' && !__builtin_strchr(part, ';'));
                for (int k = 0; match && k < n; k++) {
                    char a = fn[k], b = part[k];
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                    match = a == b;
                }
                if (match) {
                    dir = le32(buf + i + 2);
                    dir_size = le32(buf + i + 10);
                    found = 1;
                }
                i += len;
            }
        }
        if (!found) {
            return 0;
        }
    }
    *lba = dir;
    *size = dir_size;
    return 1;
}

CdlFILE *CdSearchFile(CdlFILE *fp, char *name) {
    unsigned int lba, size;
    if (!iso_lookup(name, &lba, &size)) {
        return NULL;
    }
    CdIntToPos((int)lba, &fp->pos);
    fp->size = size;
    __builtin_strncpy(fp->name, name, sizeof fp->name - 1);
    fp->name[sizeof fp->name - 1] = 0;
    return fp;
}

/* ---- FF9.IMG code overlays ------------------------------------------------------------------------------
 * Directory 0 of FF9.IMG holds the code overlays (docs/ff9img.md in the decomp). When their sectors are copied to
 * the overlay's slot, the relocated data image is put back over them (psx_overlay_patch). */
#define MAX_CODE_FILES 32
static struct {
    unsigned int lba, end, id;
} code_files[MAX_CODE_FILES];
static int n_code_files = -1;

static void scan_ff9img(void) {
    unsigned int base, size;
    u8 hdr[2048], tbl[2048];
    n_code_files = 0;
    if (!iso_lookup("\\FF9.IMG;1", &base, &size)) {
        return;
    }
    read_user(base, hdr);
    if (__builtin_memcmp(hdr, "FF9 ", 4) != 0) {
        return;
    }
    /* directory list at +0x10: {type, count, dir_id, first_sector}; dir 0 is the first entry */
    unsigned int type = le32(hdr + 0x10), count = le32(hdr + 0x14);
    if (type != 2 || count > MAX_CODE_FILES) {
        return;
    }
    /* file tables are packed from FF9.IMG sector 1, dir 0's first: {file_id, sector} pairs, then {-1, end} */
    read_user(base + 1, tbl);
    for (unsigned int i = 0; i < count && i * 8 + 12 < 2048; i++) {
        code_files[i].id = le32(tbl + i * 8);
        code_files[i].lba = base + le32(tbl + i * 8 + 4);
        code_files[i].end = base + le32(tbl + i * 8 + 12);
    }
    n_code_files = (int)count;
}

/* The code overlay `lba` belongs to, and the sector's offset in that file (bytes); 0 if none. */
static unsigned int code_file_of(unsigned int lba, unsigned int *file_off) {
    if (n_code_files < 0) {
        scan_ff9img();
    }
    for (int i = 0; i < n_code_files; i++) {
        if (lba >= code_files[i].lba && lba < code_files[i].end) {
            *file_off = (lba - code_files[i].lba) * 2048;
            return code_files[i].id;
        }
    }
    return 0;
}

/* ---- commands ------------------------------------------------------------------------------------------- */

/* a response of just the status byte: the rest of the caller's 8-byte result buffer cleared (the disc-change check
 * tests result[1] after a seek, which would otherwise be whatever its stack held) */
static void result_status(u8 *result) {
    if (result) {
        __builtin_memset(result, 0, 8);
        result[0] = status;
    }
}

/* Execute one command now. Returns the interrupt it completes with (CdlComplete for the second-response
 * commands, CdlAcknowledge otherwise). */
static int execute(u8 com, const u8 *param, u8 *result) {
    psx_cd_debug[0] = com;
    psx_cd_debug[1]++;
    if (param) {
        param = PSX_PTR(param);
    }
    switch (com) {
    case CdlSetloc:
        if (param) {
            loc = (unsigned int)CdPosToInt((CdlLOC *)param);
        }
        break;
    case CdlSetmode:
        if (param) {
            mode = param[0];
        }
        break;
    case CdlReadN:
    case CdlReadS:
        psx_cd_read_starts++;
        read_lba = loc;
        CdIntToPos((int)loc, (CdlLOC *)last_pos);
        reading = 1;
        status = STAT_MOTOR | STAT_READ;
        break;
    case CdlSeekL:
    case CdlSeekP:
        read_lba = loc;
        result_status(result);
        return CdlComplete;
    case CdlPause:
    case CdlStop:
    case CdlInit:
        reading = 0;
        status = STAT_MOTOR;
        result_status(result);
        return CdlComplete;
    case CdlGetlocL:
    case CdlGetlocP:
        if (result) {
            CdlLOC l;
            CdIntToPos((int)read_lba, &l);
            __builtin_memset(result, 0, 8);
            result[0] = l.minute, result[1] = l.second, result[2] = l.sector;
        }
        return CdlAcknowledge;
    case CdlGetTN:
        if (result) {
            result[0] = status, result[1] = 1, result[2] = 1;
        }
        return CdlAcknowledge;
    default:
        break;
    }
    result_status(result);
    return CdlAcknowledge;
}

/* libcd calls the sync callback with CdlComplete when a command finishes: on its second response for the
 * two-stage commands (seek, pause, stop, init), on the acknowledge for the others (Setloc, Setmode, ReadN, ...).
 * The game's CD state machine advances on exactly these. */
static void complete(int intr, u8 *result) {
    u8 r[8] = {status};
    last_com_done = 1;
    if (sync_cb) {
        sync_cb(CdlComplete, result ? result : r);
    }
}

static int control(u8 com, u8 *param, u8 *result) {
    if (pumping) {
        /* issued from inside a callback: run it after the callback returns */
        int next = (q_tail + 1) % QUEUE;
        if (next == q_head) {
            port_fatal("libcd command queue full");
        }
        queue[q_tail].com = com;
        if (param) {
            __builtin_memcpy(queue[q_tail].param, param, 4);
        }
        q_tail = next;
        result_status(result);
        return 1;
    }
    int intr = execute(com, param, result);
    pumping = 1;
    complete(intr, result);
    pumping = 0;
    psx_cd_pump();
    return 1;
}

int CdControl(u8 com, u8 *param, u8 *result) {
    return control(com, param, result);
}

int CdControlF(u8 com, u8 *param) {
    return control(com, param, NULL);
}

int CdControlB(u8 com, u8 *param, u8 *result) {
    return control(com, param, result);
}

static void pump(int max_sectors);

/* A loader's poll: commands complete at once, but sectors only come at the drive's rate (psx_cd_frame), so a load
 * takes the PS1's frames, through which its interrupts go on (AKAO's sound sequences, the VSync callback): a run of
 * polls lets a frame pass. (Delivering a sector at each poll loaded in no time at all: a battle's sound effect
 * still played from its data when the field's overwrote it, d1_f0310.) */
void psx_cd_pump(void) {
    pump(0);
    if (reading && ready_cb) psx_poll_wait();
}

/* One frame of drive time: 75 sectors/s at normal speed, 150 with CdlModeSpeed (mode bit 7), as whole sectors
 * with the remainder carried over. Credit only builds while reading, so seeks and idle time don't bank sectors. */
void psx_cd_frame(void) {
    static unsigned int credit; /* in 1/60 sectors */
    if (!reading || !ready_cb) {
        credit = 0;
        pump(MAX_SECTORS_PER_PUMP);
        return;
    }
    credit += (mode & 0x80) ? 150 : 75;
    int n = (int)(credit / 60);
    credit %= 60;
    pump(n > MAX_SECTORS_PER_PUMP ? n : MAX_SECTORS_PER_PUMP);
}

static void pump(int max_sectors) {
    psx_cd_debug[4] = pumping | reading << 4 | (ready_cb != NULL) << 8 | (sync_cb != NULL) << 12;
    psx_cd_debug[5] = (unsigned)(q_tail - q_head);
    if (pumping) {
        return;
    }
    pumping = 1;
    int delivered = 0;
    for (;;) {
        if (q_head != q_tail) {
            u8 com = queue[q_head].com;
            u8 param[4];
            __builtin_memcpy(param, queue[q_head].param, 4);
            q_head = (q_head + 1) % QUEUE;
            complete(execute(com, param, NULL), NULL);
            continue;
        }
        if (reading && ready_cb && delivered < max_sectors) {
            sector_lba = read_lba;
            read_raw(read_lba++, sector);
#ifdef PORT_DISC_PACK
            sector_rebuilt = sector_is_data;
#endif
            psx_cd_debug[2]++;
            psx_cd_debug[3] = sector_lba;
            sector_pos = 0;
            u8 r[8] = {status};
            ready_cb(CdlDataReady, r);
            delivered++;
            continue;
        }
        break;
    }
    pumping = 0;
}

/* CD_getsector(madr, size): copy `size` words of the current sector's user data (libcd's DMA from the drive's
 * buffer). Successive calls continue where the last one stopped. Returns 0 on success. */
int CD_getsector(void *madr, int size) {
    unsigned int bytes = (unsigned int)size * 4;
    unsigned int base = (mode & 0x20) ? 12 : USER_OFFSET; /* CdlModeSize1: 2340-byte sectors from the header */
#ifdef PORT_DISC_PACK
    if (sector_rebuilt && base + sector_pos + bytes > USER_OFFSET + 2048)
        port_fatal("disc %d: sector %u's EDC/ECC read, which discs.bin doesn't keep for game data", disc_no, sector_lba);
#endif
    __builtin_memcpy(madr, sector + base + sector_pos, bytes);
    /* user data of a code overlay landing at that overlay's slot: put the relocated image back over it */
    unsigned int file_off, id;
    if (base + sector_pos >= USER_OFFSET && (id = code_file_of(sector_lba, &file_off)) != 0) {
        static const unsigned int slots[] = PSX_OVL_SLOT_ADDRS;
        unsigned int off = (u32)madr - (u32)psx_ram;
        unsigned int at = file_off + (base + sector_pos - USER_OFFSET);
        for (unsigned int k = 0; k < 3; k++) {
            if (off < PSX_RAM_SIZE && off == ((slots[k] + at) & (PSX_RAM_SIZE - 1))) {
                psx_overlay_patch(id, off, bytes);
            }
        }
    }
    sector_pos += bytes;
    return 0;
}

int CD_getsector2(void *madr, int size) {
    return CD_getsector(madr, size);
}

/* libcd callback setters (decomp placeholders CdSyncCallback / CdReadyCallback / CdSetDebug): each stores the
 * callback and returns the previous one. */
CdCallback CdSyncCallback(CdCallback f) {
    CdCallback old = sync_cb;
    sync_cb = f;
    return old;
}

CdCallback CdReadyCallback(CdCallback f) {
    CdCallback old = ready_cb;
    ready_cb = f;
    return old;
}

CdCallback CdSetDebug(CdCallback f) {
    CdCallback old = read_cb;
    read_cb = f;
    return old;
}

/* CdLastPos, CdFlush, CdReady (decomp placeholders CdLastPos ... CdReady); CdStatus and CdMode are
 * matched C in the decomp and read the globals above. */
u8 *CdLastPos(void) {
    return last_pos;
}

void CdFlush(void) {
}

/* CdReady(mode, result): a sector is waiting (CdlDataReady) or not (CdlNoIntr). Sectors are delivered through
 * the ready callback, so a polling caller gets the pump. */
s32 CdReady(s32 m, u8 *result) {
    psx_cd_pump();
    result_status(result);
    return CdlNoIntr;
}

int CdInit(void) {
    open_disc();
    status = STAT_MOTOR;
    return 1;
}

int CdReset(int m) {
    reading = 0;
    q_head = q_tail = 0;
    return 1;
}

int CdDiskReady(int m) {
    return CdlComplete;
}

int CdGetDiskType(void) {
    return 2; /* CdlCdromFormat */
}

int CdMix(CdlATV *vol) {
    return 1;
}
