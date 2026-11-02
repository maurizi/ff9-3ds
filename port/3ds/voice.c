/* Voice acting (optional): a dialogue line's recording, played while its window is up.
 *
 * FF9 has no voices; a voice pack adds them. port/game/voice_hook.c names each line the game shows (its text bank
 * and message number) and calls ctr_voice_line; when the pack has a file for it,
 *     sdmc:/3ds/ff9/voice/<bank>/<message>.opus
 * it plays on its own NDSP channel (Opus, any rate libopusfile decodes: 48 kHz), the music ducked under it, until
 * the line ends or its window closes. Decoding runs on a thread on the second core. No pack, no cost. */
#include <3ds.h>
#include <opus/opusfile.h>
#include <stdio.h>
#include <string.h>

#include "ctr.h"
#include "psx_runtime.h"

#define CHN 1
#define NBUF 4
#define BUF_SAMPLES 4096 /* stereo frames at 48 kHz: 85 ms */
#define DUCK 0.45f       /* the music's volume under a voice */

static ndspWaveBuf wbuf[NBUF];
static s16 *pcm;
static Thread thread;
static LightEvent kick;
static LightLock lock;
static volatile int quit, stop_req;
static char next_path[96];
static int have_next, playing, available = -1;

static void duck(int on) {
    float mix[12] = {on ? DUCK : 1.0f, on ? DUCK : 1.0f};
    ndspChnSetMix(0, mix); /* (audio.c's channel: the SPU's mix) */
}

static void play(const char *path) {
    int err = 0;
    OggOpusFile *f = op_open_file(path, &err);
    if (!f) {
        port_log("voice: %s: not Opus (%d)\n", path, err);
        return;
    }
    ndspChnReset(CHN);
    ndspChnSetInterp(CHN, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHN, 48000.0f);
    ndspChnSetFormat(CHN, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(CHN, mix);
    for (int i = 0; i < NBUF; i++) wbuf[i].status = NDSP_WBUF_DONE;
    playing = 1;
    duck(1);
    u64 t0 = osGetTime();
    int ended = 0, next = 0;
    while (!quit && !stop_req) {
        ndspWaveBuf *b = &wbuf[next];
        if (b->status == NDSP_WBUF_DONE || b->status == NDSP_WBUF_FREE) {
            if (ended) {
                int busy = 0;
                for (int i = 0; i < NBUF; i++) busy |= wbuf[i].status == NDSP_WBUF_QUEUED || wbuf[i].status == NDSP_WBUF_PLAYING;
                if (!busy) break;
                svcSleepThread(10000000);
                continue;
            }
            s16 *out = pcm + next * BUF_SAMPLES * 2;
            int n = 0;
            while (n < BUF_SAMPLES) { /* (stereo whatever the file: op_read_stereo up- or downmixes) */
                int r = op_read_stereo(f, out + n * 2, (BUF_SAMPLES - n) * 2);
                if (r <= 0) { ended = 1; break; }
                n += r;
            }
            if (n) {
                b->data_vaddr = out;
                b->nsamples = (u32)n;
                DSP_FlushDataCache(out, (u32)n * 4);
                ndspChnWaveBufAdd(CHN, b);
                next = (next + 1) % NBUF;
            }
        } else {
            LightEvent_WaitTimeout(&kick, 20000000);
        }
    }
    ndspChnReset(CHN);
    (void)t0;
#ifdef PORT_VOICE_LOG
    port_log("voice: %s %s after %u ms\n", path, ended ? "played" : "stopped", (unsigned)(osGetTime() - t0));
#endif
    op_free(f);
    playing = 0;
    duck(0);
}

static void voice_main(void *unused) {
    (void)unused;
    while (!quit) {
        LightEvent_Wait(&kick);
        if (quit) break;
        LightLock_Lock(&lock);
        char path[96];
        int go = have_next;
        if (go) memcpy(path, next_path, sizeof path), have_next = 0;
        stop_req = 0;
        LightLock_Unlock(&lock);
        if (go) play(path);
    }
}

/* whether a voice pack is on the card (looked for once) */
int port_setting_get(int k);
static int pack_present(void) {
    if (!port_setting_get(2)) return 0; /* (the "voices" option off) */
    if (available < 0) {
        FILE *f = fopen("sdmc:/3ds/ff9/voice/pack.txt", "r");
        available = f != NULL;
        if (f) fclose(f);
        if (available) port_log("voice: pack found\n");
    }
    return available;
}

/* A dialogue line is shown (bank: its text bank, message: its number in it): its recording, if the pack has one. */
void ctr_voice_line(unsigned bank, unsigned message) {
    if (!pack_present()) return;
    char path[96];
    snprintf(path, sizeof path, "sdmc:/3ds/ff9/voice/%u/%u.opus", bank, message);
    FILE *f = fopen(path, "rb");
    if (!f) { ctr_voice_stop(); return; }
    fclose(f);
    if (!thread) {
        pcm = linearAlloc(NBUF * BUF_SAMPLES * 4);
        if (!pcm) return;
        LightEvent_Init(&kick, RESET_ONESHOT);
        LightLock_Init(&lock);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        thread = threadCreate(voice_main, NULL, 32 * 1024, prio - 1, ctr_is_new3ds() ? 2 : 1, false);
        if (!thread) return;
    }
    LightLock_Lock(&lock);
    memcpy(next_path, path, sizeof path);
    have_next = 1;
    stop_req = playing; /* (a line already playing gives way) */
    LightLock_Unlock(&lock);
    LightEvent_Signal(&kick);
}

/* The line's window closed (or another without a recording opened). */
void ctr_voice_stop(void) {
    if (!thread || !playing) return;
    stop_req = 1;
    LightEvent_Signal(&kick);
}

void ctr_voice_exit(void) {
    if (!thread) return;
    quit = 1;
    stop_req = 1;
    LightEvent_Signal(&kick);
    threadJoin(thread, 1000000000ULL); /* (a second at most: closing must not hang) */
    threadFree(thread);
    thread = NULL;
}
