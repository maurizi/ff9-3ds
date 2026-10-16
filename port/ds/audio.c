/* ARM9 PCM producer -> IRQ queue consumer -> Maxmod ARM7 sound channels 4/5.
 * Full 44.1 kHz SPU PCM is retained; starvation inserts measured silence instead
 * of replaying stale buffers or discarding later game samples. */
#include <nds.h>
#include <stdio.h>
#include "audio.h"
#include "pcm_ring.h"
#include "platform.h"
#include "psx_spu.h"
volatile uint32_t port_audio_stats[12] = {0x41554431};
#ifdef PORT_AUDIO
#include <maxmod9.h>
static PortPcmRing queue;
#ifdef PORT_AUDIO_CAPTURE
int16_t port_audio_capture[PORT_AUDIO_CAPTURE * 2];
volatile uint32_t port_audio_captured_frames;
#endif
static int initialized, streaming;
enum { STREAM_FRAMES = 2048, PREFILL_FRAMES = 4096 };

static mm_word refill(mm_word length, mm_addr destination, mm_stream_formats format) {
    if (format != MM_STREAM_16BIT_STEREO) {
        __builtin_memset(destination, 0, length * 4);
        ++port_audio_stats[9];
        return length;
    }
    port_pcm_pull(&queue, destination, length);
    port_audio_stats[3] = queue.read;
    port_audio_stats[4] = queue.underrun;
    port_audio_stats[5] = queue.callbacks;
    return length;
}
int port_audio_init(void) {
    if (initialized) return 1;
    port_pcm_reset(&queue);
#ifdef PORT_AUDIO_CAPTURE
    port_audio_captured_frames = 0;
#endif
    for (unsigned i = 1; i < 12; ++i) port_audio_stats[i] = 0;
    if (!mmInitNoSoundbank()) { port_audio_stats[1] = 4; return 0; }
    initialized = 1;
    port_audio_stats[1] = 1;
    port_audio_stats[8] = 44100;
#ifdef PORT_AUDIO_HASH
    port_audio_stats[10] = 2166136261u;
#endif
    psx_spu_set_rendering(1);
    return 1;
}
void port_audio_stop(void) {
    psx_spu_set_rendering(0);
    if (streaming) mmStreamClose();
    streaming = 0;
    initialized = 0;
    port_audio_stats[1] = 3;
}
unsigned port_audio_queued(void) { return port_pcm_available(&queue); }
void plat_audio_samples(const short *samples, unsigned frames) {
    if (!initialized) return;
    while (frames) {
        unsigned count = port_pcm_push(&queue, samples, frames);
#ifdef PORT_AUDIO_CAPTURE
        unsigned captured = port_audio_captured_frames;
        unsigned copy = PORT_AUDIO_CAPTURE - captured;
        if (copy > count) copy = count;
        __builtin_memcpy(port_audio_capture + captured * 2, samples, copy * 4);
        port_audio_captured_frames = captured + copy;
#endif
#ifdef PORT_AUDIO_HASH
        for (unsigned i = 0; i < count * 2; ++i) {
            port_audio_stats[10] = (port_audio_stats[10] ^ (uint16_t)samples[i]) * 16777619u;
            port_audio_stats[11] += samples[i] != 0;
        }
#endif
        samples += count * 2;
        frames -= count;
        port_audio_stats[2] = queue.written;
        unsigned queued = port_pcm_available(&queue);
        if (queued > port_audio_stats[6]) port_audio_stats[6] = queued;
        if (!streaming && queued >= PREFILL_FRAMES) {
            mm_stream stream = {.sampling_rate = 44100, .buffer_length = STREAM_FRAMES,
                .callback = refill, .format = MM_STREAM_16BIT_STEREO,
                .timer = MM_TIMER0, .manual = false};
            mmStreamOpen(&stream);
            if (!(TIMER_CR(0) & TIMER_ENABLE)) {
                port_audio_stop(); port_audio_stats[1] = 4;
                printf("PCM stream failed to open\n");
                return;
            }
            streaming = 1;
            port_audio_stats[1] = 2;
        }
        if (frames && !count) {
            ++port_audio_stats[7];
            swiIntrWait(1, IRQ_TIMER0); /* foreground only; consumer stays live */
        }
    }
}
#else
int port_audio_init(void) { return 0; }
void port_audio_stop(void) {}
unsigned port_audio_queued(void) { return 0; }
void plat_audio_samples(const short *samples, unsigned count) { (void)samples; (void)count; }
#endif
