/* DS hardware mixing for the emulated SPU (docs/design.md §7.0, HWSOUND=1).
 *
 * The SPU core steps the voices the game observes without mixing them (psx_spu_set_hw). Each voice's decoded
 * ADPCM blocks arrive here in play order and go into the voice's ring buffer, which a DS channel plays in a
 * loop at the voice's sample rate. A key-on restarts the ring; the channel starts at the next frame, when that
 * frame's samples are in. Once a frame, each sounding voice's envelope, volumes and main volume set its channel's
 * volume and pan, and its pitch the frequency. The first 16 sounding voices get channels.
 */
#ifdef PORT_HW_SOUND
#include <nds.h>
#include <string.h>
#include "psx_spu.h"
#include "audio_hw.h"

#define VOICES 24
#define RING 4096          /* samples per voice ring: ~93 ms at 44.1 kHz, longer at lower pitches */
#define LEAD 1             /* frames of samples written before a channel starts */

typedef struct {
    s16 *ring;
    unsigned write;        /* samples written since the key-on */
    int restart;           /* key-on seen: the channel (re)starts next frame */
    int channel;           /* DS channel, -1 none */
    int sound_id;          /* libnds id, -1 none */
    u8 volume, pan;
    u16 freq;
    int flush_from;        /* first ring sample not yet flushed from the data cache */
    int lost;              /* sounding but without a channel (quieter than 16 others) */
} Voice;

static Voice voices[VOICES];
static s16 rings[VOICES][RING] __attribute__((aligned(32)));
static u32 channel_used; /* bit per DS channel */
int port_hw_sound_stats[4];

static void key_on(unsigned v, void *user) {
    (void)user;
    Voice *p = &voices[v];
    p->write = 0;
    p->flush_from = 0;
    p->restart = 1;
    p->lost = 0;
}

static void block(unsigned v, const int16_t *samples, void *user) {
    (void)user;
    Voice *p = &voices[v];
    unsigned at = p->write % RING;
    unsigned first = RING - at < 28 ? RING - at : 28;
    memcpy(p->ring + at, samples, first * 2);
    if (first < 28) memcpy(p->ring, samples + first, (28 - first) * 2);
    p->write += 28;
}

static const PsxSpuSink sink = {key_on, block, NULL};

void port_hw_sound_init(void) {
    soundEnable();
    for (int i = 0; i < VOICES; ++i)
        voices[i] = (Voice){rings[i], 0, 0, -1, -1, 0, 64, 0, 0, 0};
    psx_spu_set_hw(&sink);
}

static void stop(Voice *p) {
    if (p->sound_id >= 0) soundKill(p->sound_id);
    if (p->channel >= 0) channel_used &= ~(1u << p->channel);
    p->sound_id = p->channel = -1;
}

/* Once per emulated frame, after the SPU advanced. When more than 16 voices sound, the loudest 16 have channels. */
void port_hw_sound_frame(void) {
    static PsxSpuVoiceOutput outs[VOICES];
    static int amps[VOICES];
    int candidates = 0;
    for (int v = 0; v < VOICES; ++v) {
        PsxSpuVoiceOutput *out = &outs[v];
        psx_spu_voice_output(v, out);
        int l = out->left < 0 ? -out->left : out->left, r = out->right < 0 ? -out->right : out->right;
        amps[v] = out->on && !out->noise ? (l > r ? l : r) + 1 : 0;
        candidates += amps[v] > 0;
    }
    /* the 16th largest amplitude: voices below it give their channel up */
    int cut = 1;
    if (candidates > 16) {
        int sorted[VOICES], n = 0;
        for (int v = 0; v < VOICES; ++v) if (amps[v]) sorted[n++] = amps[v];
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && sorted[j - 1] < sorted[j]; --j) { int t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t; }
        cut = sorted[15];
    }
    int sounding = 0, granted = 0;
    for (int v = 0; v < VOICES; ++v) {
        Voice *p = &voices[v];
        PsxSpuVoiceOutput *out = &outs[v];
        int keep = amps[v] && amps[v] >= cut && granted < 16;
        if (!keep) {
            if (amps[v]) port_hw_sound_stats[1]++;
            if (amps[v]) p->lost = 1; /* still sounding without a channel: its ring falls behind */
            stop(p);
            continue;
        }
        ++granted;
        int l = out->left < 0 ? -out->left : out->left, r = out->right < 0 ? -out->right : out->right;
        int vol = (amps[v] - 1) * 127 / 0x4000;
        if (vol > 127) vol = 127;
        int pan = l + r ? 64 + 63 * (r - l) / (l + r) : 64;
        unsigned rate = 44100u * out->pitch / 4096;
        u16 freq = rate > 65535 ? 65535 : rate < 1 ? 1 : (u16)rate;
        /* make this frame's samples visible to the sound DMA */
        if (p->write) {
            unsigned from = p->flush_from % RING, to = p->write % RING;
            if (p->write - p->flush_from >= RING) DC_FlushRange(p->ring, RING * 2);
            else if (from <= to) DC_FlushRange(p->ring + from, (to - from) * 2 + 32);
            else { DC_FlushRange(p->ring + from, (RING - from) * 2); DC_FlushRange(p->ring, to * 2 + 32); }
            p->flush_from = p->write;
        }
        if (p->restart) {
            stop(p);
            p->restart = 0;
        }
        if (p->sound_id < 0 && p->lost) {
            /* back from losing its channel: start over from the samples written from now on */
            p->lost = 0;
            p->write = p->flush_from = 0;
            continue;
        }
        if (p->sound_id < 0) {
            if (!p->write) continue;
            int ch = -1;
            for (int c = 0; c < 16; ++c) if (!(channel_used & (1u << c))) { ch = c; break; }
            if (ch < 0) continue;
            p->channel = ch;
            channel_used |= 1u << ch;
            p->sound_id = soundPlaySampleChannel(ch, p->ring, SoundFormat_16Bit, RING * 2, freq, (u8)vol, (u8)pan,
                                                 true, 0);
            p->volume = (u8)vol; p->pan = (u8)pan; p->freq = freq;
            port_hw_sound_stats[0]++;
        } else {
            if (vol != p->volume) { soundSetVolume(p->sound_id, (u8)vol); p->volume = (u8)vol; }
            if (pan != p->pan) { soundSetPan(p->sound_id, (u8)pan); p->pan = (u8)pan; }
            if (freq != p->freq) { soundSetFreq(p->sound_id, freq); p->freq = freq; }
        }
        ++sounding;
    }
    port_hw_sound_stats[2] = sounding;
}
#endif
