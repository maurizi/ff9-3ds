/* Sound out: the SPU mix (44.1 kHz stereo PCM16, plat_audio_samples) queued on one NDSP channel.
 *
 * The mix is produced in the game's virtual time, a VBlank's worth (~735 frames) at a time. It is cut into
 * fixed-size wave buffers; when the DSP hasn't finished the buffer we would fill next, the samples are dropped
 * (the game is running ahead of real time), and when the queue runs dry the DSP plays silence. */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "ctr.h"
#include "platform.h"
#include "psx_runtime.h"

#define NBUF 12 /* (~280 ms queued: rides over the game's short stalls, as loading into a battle) */
#define BUF_FRAMES 1024

static ndspWaveBuf wbuf[NBUF];
static s16 *pcm;
static int ok, cur, fill;
unsigned int ctr_audio_dropped, ctr_audio_underruns, ctr_audio_frames, ctr_audio_peak; /* (the heartbeat's) */

static int no_dsp; /* ndspInit failed: the DSP firmware isn't on the SD card (field_hud.c tells the player at boot) */
int port_audio_missing(void) { return no_dsp; }

void ctr_audio_init(void) {
    if (R_FAILED(ndspInit())) {
        port_log("audio: ndspInit failed (no sdmc:/3ds/dspfirm.cdc? dump it with DSP1): silent\n");
        no_dsp = 1;
        return;
    }
    pcm = linearAlloc(NBUF * BUF_FRAMES * 4);
    if (!pcm) return;
    memset(pcm, 0, NBUF * BUF_FRAMES * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 44100.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix);
    memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NBUF; i++) {
        wbuf[i].data_vaddr = pcm + i * BUF_FRAMES * 2;
        wbuf[i].nsamples = BUF_FRAMES;
        wbuf[i].status = NDSP_WBUF_DONE;
    }
    ok = 1;
    port_log("audio: NDSP, 44.1 kHz stereo\n");
}

void ctr_audio_exit(void) {
    if (!ok) return;
    ndspChnReset(0);
    ndspExit();
    linearFree(pcm);
    ok = 0;
}

void plat_audio_samples(const short *samples, unsigned frames) {
    ctr_audio_frames += frames;
    for (unsigned i = 0; i < frames * 2; i += 16) { /* (a sample in 8: the level the heartbeat reports) */
        unsigned v = (unsigned)(samples[i] < 0 ? -samples[i] : samples[i]);
        if (v > ctr_audio_peak) ctr_audio_peak = v;
    }
    if (!ok) return;
    while (frames) {
        ndspWaveBuf *b = &wbuf[cur];
        if (b->status != NDSP_WBUF_DONE && b->status != NDSP_WBUF_FREE) {
            ctr_audio_dropped += frames; /* the DSP is a whole queue behind: drop, stay in real time */
            return;
        }
        unsigned n = BUF_FRAMES - fill;
        if (n > frames) n = frames;
        memcpy((s16 *)b->data_vaddr + fill * 2, samples, n * 4);
        samples += n * 2;
        frames -= n;
        fill += n;
        if (fill == BUF_FRAMES) {
            int queued = 0;
            for (int i = 0; i < NBUF; i++) queued += wbuf[i].status == NDSP_WBUF_QUEUED || wbuf[i].status == NDSP_WBUF_PLAYING;
            if (!queued) ctr_audio_underruns++;
            DSP_FlushDataCache(b->data_vaddr, BUF_FRAMES * 4);
            ndspChnWaveBufAdd(0, b);
            /* dynamic rate control: the game makes 44100 samples per 60 of its frames, the 3DS paces it to its own
             * refresh (59.83 Hz) and may run a little slow besides; play a fraction of a percent slower or faster to
             * keep the queue about half full instead of running dry (a stutter) or full (dropped samples) */
            float r = 1.0f + 0.01f * (float)(queued + 1 - NBUF / 2) / (float)(NBUF / 2);
            if (r < 0.985f) r = 0.985f;
            if (r > 1.005f) r = 1.005f;
            ndspChnSetRate(0, 44100.0f * r);
            cur = (cur + 1) % NBUF;
            fill = 0;
        }
    }
}
