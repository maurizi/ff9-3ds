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

#define NBUF 6
#define BUF_FRAMES 1024

static ndspWaveBuf wbuf[NBUF];
static s16 *pcm;
static int ok, cur, fill;
unsigned int ctr_audio_dropped, ctr_audio_underruns;

void ctr_audio_init(void) {
    if (R_FAILED(ndspInit())) {
        printf("audio: ndspInit failed (no dspfirm.cdc?): silent\n");
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
}

void ctr_audio_exit(void) {
    if (!ok) return;
    ndspChnReset(0);
    ndspExit();
    linearFree(pcm);
    ok = 0;
}

void plat_audio_samples(const short *samples, unsigned frames) {
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
            cur = (cur + 1) % NBUF;
            fill = 0;
        }
    }
}
