/* PC host platform (32-bit Linux): the same game code and port/psx layer, headless, for fast iteration, gdb and
 * lockstep comparison with PCSX-Redux.
 *
 * psx_ram is placed at 0x02000000 by the link (--defsym) and mapped here before main, so PS1 pointers in the
 * game's ordering tables rebuild as on the DS (0x02000000 | addr24).
 *
 * Usage: ff9ds [--data DIR] [--frames N] [--status EVERY_N_FRAMES (0: off)] [--press BUTTON@FRAME[+FRAMES]]...
 *              [--watch ADDR:SIZE]... [--render] [--dump EVERY_N_FRAMES]
 * --render runs the software reference GPU (port/psx/softgpu.c); --dump also writes the displayed area as
 * build/frames/frame_NNNNNN.ppm (--dump-dir DIR: in DIR).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "input_script.h"
#include "platform.h"
#include "psx_mem.h"
#include "psx_runtime.h"
#include "psx_spu.h"
#include "psx_spu_trace.h"

static const char *data_dir = "disks/us";
static long frame_limit = -1;
static long spu_snapshot_at = -1;
static long ram_dump_at = -1; /* --ram-dump FRAME:PATH: PS1 RAM image, for tools/ram_diff.py */
/* --hw-sound: the DS's hardware-mixing SPU path (design §7.0) with a sink that only counts, to check that the game
 * observes the same SPU as with mixing. */
static int hw_sound;
static unsigned long hw_blocks, hw_key_ons;
static void hw_key_on(unsigned voice, void *user) { (void)voice; (void)user; hw_key_ons++; }
static void hw_block(unsigned voice, const int16_t *samples, void *user) { (void)voice; (void)samples; (void)user; hw_blocks++; }
static const PsxSpuSink hw_counting_sink = {hw_key_on, hw_block, NULL, NULL};
static void hw_report(void) { fprintf(stderr, "hw sound: %lu key-ons, %lu blocks\n", hw_key_ons, hw_blocks); }
static const char *ram_dump_path;
static const char *spu_snapshot_path;
/* After the snapshot, the next SPU_REPLAY_FRAMES frames of register writes and RAM uploads follow it in the
 * file, by output sample: {u32 sample, type, a, b, b bytes of RAM data padded to 4}, then sample 0xffffffff. */
enum { SPU_REPLAY_FRAMES = 120 };
static FILE *spu_replay;
static unsigned long spu_replay_samples;
static void spu_replay_u32(unsigned v) {
    unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24};
    fwrite(b, 1, 4, spu_replay);
}
static void spu_replay_event(unsigned type, unsigned a, unsigned b, const void *data) {
    if (type == PSX_SPU_TRACE_SAMPLE) { spu_replay_samples++; return; }
    if (type != PSX_SPU_TRACE_WRITE && type != PSX_SPU_TRACE_RAM) return;
    spu_replay_u32(spu_replay_samples); spu_replay_u32(type); spu_replay_u32(a); spu_replay_u32(b);
    if (type == PSX_SPU_TRACE_RAM) {
        static const unsigned char pad[3];
        fwrite(data, 1, b, spu_replay);
        fwrite(pad, 1, (4 - b % 4) % 4, spu_replay);
    }
}
static void spu_replay_close(void) {
    if (!spu_replay) return;
    psx_spu_set_trace(NULL);
    spu_replay_u32(0xffffffffu);
    if (fclose(spu_replay)) perror("spu snapshot");
    spu_replay = NULL;
}
static long frames;
static long status_every = 60;
static long dump_every; /* --dump N: write the displayed area every N frames to build/frames/ */
static const char *dump_dir = "build/frames"; /* --dump-dir DIR: elsewhere (parallel runs) */
static long vram_at = -1; /* --vram FRAME: also write all of VRAM at that frame */
int plat_spu_trace_open(const char *path);
static FILE *audio_file;
static unsigned long long audio_frames;

static void audio_u32(unsigned value) {
    unsigned char bytes[4] = {value, value >> 8, value >> 16, value >> 24};
    fwrite(bytes, 1, 4, audio_file);
}

static void audio_header(void) {
    rewind(audio_file);
    fwrite("RIFF", 1, 4, audio_file);
    audio_u32((unsigned)(audio_frames * 4 + 36));
    fwrite("WAVEfmt ", 1, 8, audio_file);
    audio_u32(16);
    audio_u32(0x00020001); /* PCM, stereo */
    audio_u32(44100);
    audio_u32(44100 * 4);
    audio_u32(0x00100004); /* block align 4, bits/sample 16 */
    fwrite("data", 1, 4, audio_file);
    audio_u32((unsigned)(audio_frames * 4));
}

static void close_audio(void) {
    if (!audio_file) return;
    audio_header();
    if (fclose(audio_file)) fprintf(stderr, "audio capture close failed\n");
    audio_file = NULL;
}

void plat_audio_samples(const short *samples, unsigned count) {
    if (!audio_file) return;
    if ((audio_frames + count) * 4 > 0xffffffffu - 36 ||
        fwrite(samples, sizeof(short) * 2, count, audio_file) != count) {
        fprintf(stderr, "audio capture write failed or WAV size exceeded\n");
        exit(2);
    }
    audio_frames += count;
}

void psx_softgpu_enable(int on);
extern unsigned short *psx_vram;

#include "psx_display.h"

/* A VRAM area as a binary PPM (15-bit BGR expanded to 24-bit RGB): the displayed one, or all of it. */
static void dump_area(const char *name, int x0, int y0, int w, int h, int rgb24) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s_%06ld.ppm", dump_dir, name, frames);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned short c = psx_vram[((y0 + y) & 511) * 1024 + ((x0 + x) & 1023)];
            unsigned char rgb[3] = {(unsigned char)((c & 31) << 3), (unsigned char)(((c >> 5) & 31) << 3),
                                    (unsigned char)(((c >> 10) & 31) << 3)};
            if (rgb24) {
                unsigned packed = psx_display_rgb24(psx_vram, x0, y0 + y, x);
                rgb[0] = packed; rgb[1] = packed >> 8; rgb[2] = packed >> 16;
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void dump_frame(void) {
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    dump_area("frame", d.x, d.y, d.w ? d.w : 320, d.h ? d.h : 240, d.rgb24);
}

/* PS1 RAM the DS reuses because the game never touches it once it runs (port/psx/mem.c: the BIOS's RAM above 0x2000;
 * docs/ds-mode.md): here it faults instead, so any scene that does shows. */
void plat_guard_unused_ram(int on) {
    int prot = on ? PROT_NONE : PROT_READ | PROT_WRITE;
    if (mprotect(psx_ram + 0x2000, 0xE000, prot)) {
        perror("plat_guard_unused_ram");
        exit(1);
    }
}

__attribute__((constructor(101))) static void map_psx_ram(void) {
    void *p = mmap(psx_ram, PSX_RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p != (void *)psx_ram) {
        fprintf(stderr, "can't map psx_ram at %p\n", (void *)psx_ram);
        exit(1);
    }
}

extern unsigned char *g_gameState; /* PS1 0x8006794C: GameState * */

unsigned int plat_pad_buttons(int port) {
    return port == 0 ? input_script_buttons(frames) : 0;
}

/* --watch ADDR:SIZE: PS1 memory to print with each status line */
static struct {
    unsigned int addr, size;
} watches[16];
static int n_watches;

void plat_wait_frame(void) {
    frames++;
    if (dump_every && frames % dump_every == 0) {
        dump_frame();
    }
    if (frames == vram_at) {
        dump_area("vram", 0, 0, 1024, 512, 0);
    }
    if (status_every && frames % status_every == 0) {
        unsigned char *gs = g_gameState;
        fprintf(stderr, "[frame %6ld] mode %d  overlays %02X/%02X/%02X", frames, gs ? gs[8] : -1,
                psx_ovl_resident[0], psx_ovl_resident[1], psx_ovl_resident[2]);
        for (int i = 0; i < n_watches; i++) {
            const unsigned char *p = psx_ram + (watches[i].addr & (PSX_RAM_SIZE - 1));
            unsigned int v = watches[i].size == 1 ? p[0] : watches[i].size == 2 ? *(const unsigned short *)p
                                                                                  : *(const unsigned int *)p;
            fprintf(stderr, "  %08X=%X", watches[i].addr, v);
        }
        fprintf(stderr, "\n");
    }
    if (frames == spu_snapshot_at) {
        /* SPU state followed by the 512 KiB SPU RAM, for make spu-snapshot-ds */
        extern unsigned char *psx_spu_ram;
        static unsigned char state[1 << 16];
        if (psx_spu_snapshot_size() > sizeof state) { fprintf(stderr, "snapshot too large\n"); exit(1); }
        psx_spu_snapshot(state);
        FILE *f = fopen(spu_snapshot_path, "wbx");
        if (!f || fwrite(state, 1, psx_spu_snapshot_size(), f) != psx_spu_snapshot_size() ||
            fwrite(psx_spu_ram, 1, 0x80000, f) != 0x80000) {
            perror("spu snapshot");
            exit(1);
        }
        spu_replay = f;
        atexit(spu_replay_close);
        psx_spu_set_trace(spu_replay_event);
        fprintf(stderr, "SPU snapshot at frame %ld: %s\n", frames, spu_snapshot_path);
    }
    if (spu_replay && frames == spu_snapshot_at + SPU_REPLAY_FRAMES) spu_replay_close();
    if (frames == ram_dump_at) {
        plat_guard_unused_ram(0);
        FILE *f = fopen(ram_dump_path, "wbx");
        if (!f || fwrite(psx_ram, 1, PSX_RAM_SIZE, f) != PSX_RAM_SIZE || fclose(f)) {
            perror("ram dump");
            exit(1);
        }
        fprintf(stderr, "PS1 RAM at frame %ld: %s\n", frames, ram_dump_path);
    }
    if (frame_limit >= 0 && frames >= frame_limit) {
        extern unsigned int psx_gpu_scene_prims_max;
        fprintf(stderr, "frame limit %ld reached (most primitives in a scene: %u)\n", frame_limit, psx_gpu_scene_prims_max);
        exit(0);
    }
}

void plat_vlog(const char *fmt, va_list ap) {
    vfprintf(stderr, fmt, ap);
}

void plat_halt(void) {
    exit(2);
}

/* Optional production GX backend linked by the host allocation probe. */
void render_gx_init(void) __attribute__((weak));
void plat_video_init(void) {
    if (render_gx_init) render_gx_init();
}

const char *plat_data_dir(void) {
    return data_dir;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (!strcmp(argv[i], "--spu-trace") && i + 1 < argc) {
            if (!plat_spu_trace_open(argv[++i])) return 1;
        } else if (!strcmp(argv[i], "--audio") && i + 1 < argc && !audio_file) {
            audio_file = fopen(argv[++i], "wbx");
            if (!audio_file) { perror("audio capture"); return 1; }
            atexit(close_audio);
            audio_header();
            psx_spu_set_rendering(1);
        } else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) {
            port_scenario_load(argv[++i]);
        } else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            if (!input_script_add(argv[++i])) {
                fprintf(stderr, "bad --press %s (BUTTON@FRAME[+FRAMES])\n", argv[i]);
                return 1;
            }
        } else if (!strcmp(argv[i], "--watch") && i + 1 < argc && n_watches < 16) {
            if (sscanf(argv[++i], "%x:%u", &watches[n_watches].addr, &watches[n_watches].size) != 2) {
                fprintf(stderr, "bad --watch %s (ADDR:SIZE)\n", argv[i]);
                return 1;
            }
            n_watches++;
        } else if (!strcmp(argv[i], "--render")) {
            psx_softgpu_enable(1);
            { void occ_stats_install(void); occ_stats_install(); } /* (FF9_OCC=1: occ_stats.c) */
        } else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            dump_every = atol(argv[++i]);
            psx_softgpu_enable(1);
            if (system("mkdir -p build/frames") != 0) {
                return 1;
            }
        } else if (!strcmp(argv[i], "--trace-gpu")) {
            extern int psx_trace_gpu;
            psx_trace_gpu = 1;
        } else if (!strcmp(argv[i], "--vram") && i + 1 < argc) {
            vram_at = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
            status_every = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--spu-snapshot") && i + 1 < argc) {
            static char path[512];
            if (sscanf(argv[++i], "%ld:%511s", &spu_snapshot_at, path) != 2) {
                fprintf(stderr, "bad --spu-snapshot %s (FRAME:NEW_PATH)\n", argv[i]);
                return 1;
            }
            spu_snapshot_path = path;
        } else if (!strcmp(argv[i], "--hw-sound")) {
            hw_sound = 1;
        } else if (!strcmp(argv[i], "--ram-dump") && i + 1 < argc) {
            static char path[512];
            if (sscanf(argv[++i], "%ld:%511s", &ram_dump_at, path) != 2) {
                fprintf(stderr, "bad --ram-dump %s (FRAME:NEW_PATH)\n", argv[i]);
                return 1;
            }
            ram_dump_path = path;
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frame_limit = atol(argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--data DIR] [--scenario PATH] [--frames N] [--status N] [--watch ADDR:SIZE] [--press BUTTON@FRAME[+N]] [--render] [--dump N] [--audio NEW.wav] [--spu-trace NEW.bin] [--spu-snapshot FRAME:NEW_PATH] [--ram-dump FRAME:NEW_PATH]\n", argv[0]);
            return 1;
        }
    }
    /* The SPU always runs, as on the DS: the game reads it back (AKAO frees a voice when SpuGetVoiceEnvelope
     * reports its envelope finished), so a silent SPU would run different game logic. --audio only adds the WAV. */
    psx_spu_set_rendering(1);
    if (hw_sound) { psx_spu_set_hw(&hw_counting_sink); atexit(hw_report); }
    psx_run_game();
}
