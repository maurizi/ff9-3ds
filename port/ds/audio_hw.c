/* DS hardware mixing for the emulated SPU (docs/design.md §7.0, HWSOUND=1).
 *
 * The SPU core steps the voices the game observes without mixing them (psx_spu_set_hw). Each voice's decoded
 * ADPCM blocks arrive here in play order and go into the voice's ring buffer, which a DS channel plays in a
 * loop at the voice's sample rate. A key-on restarts the ring; the channel starts at the next frame, when that
 * frame's samples are in. Once a frame, each sounding voice's envelope, volumes and main volume set its channel's
 * volume and pan, and its pitch the frequency. The first 16 sounding voices get channels.
 *
 * At most 16 voices can play, so there are 16 rings, not one per voice: a voice holds one from its key-on while it
 * sounds. One that finds none, or loses its channel, plays from its next samples once it gets both again, as a
 * voice that lost its channel always did.
 */
#if defined(PORT_HW_SOUND) && (!defined(PORT_SPU_ARM7) || defined(ARM7)) /* (the ARM7's with the SPU there) */
#include <nds.h>
#include <string.h>
#include "psx_spu.h"
#include "audio_hw.h"
#ifdef ARM7
/* On the ARM7 (spu7.h) the channels are its own registers: libnds's ARM9 calls as register writes, a sound id being
 * its channel; no cache to flush. */
static int ch_play(int ch, const void *data, u32 bytes, u16 freq, u8 vol, u8 pan) {
    REG_SOUNDXCNT(ch) = 0;
    REG_SOUNDXSAD(ch) = (u32)data;
    REG_SOUNDXPNT(ch) = 0;
    REG_SOUNDXLEN(ch) = bytes >> 2;
    REG_SOUNDXTMR(ch) = SOUNDXTMR_FREQ(freq);
    REG_SOUNDXCNT(ch) = SOUNDXCNT_ENABLE | SOUNDXCNT_VOL_MUL(vol) | SOUNDXCNT_PAN(pan) | SOUNDXCNT_FORMAT_16BIT |
                        SOUNDXCNT_REPEAT;
    return ch;
}
static int noise_play(int ch, u16 freq, u8 vol, u8 pan) {
    REG_SOUNDXCNT(ch) = SOUNDXCNT_ENABLE | SOUNDXCNT_VOL_MUL(vol) | SOUNDXCNT_PAN(pan) | SOUNDXCNT_FORMAT_PSG;
    REG_SOUNDXTMR(ch) = SOUNDXTMR_FREQ(freq);
    return ch;
}
static void capture_start(void *buffer, u16 words, int unit) {
    if (unit == 0) { REG_SNDCAP0DAD = (u32)buffer; REG_SNDCAP0LEN = words; REG_SNDCAP0CNT = SNDCAPCNT_START_BUSY; }
    else { REG_SNDCAP1DAD = (u32)buffer; REG_SNDCAP1LEN = words; REG_SNDCAP1CNT = SNDCAPCNT_START_BUSY; }
}
#define soundEnable() ((void)0)
#define soundPlaySampleChannel(ch, data, fmt, bytes, freq, vol, pan, loop, point) ch_play(ch, data, bytes, freq, vol, pan)
#define soundPlayNoiseChannel(ch, freq, vol, pan) noise_play(ch, freq, vol, pan)
#define soundKill(id) (REG_SOUNDXCNT(id) = 0)
#define soundSetVolume(id, v) (REG_SOUNDXCNT(id) = (REG_SOUNDXCNT(id) & ~0x7Fu) | SOUNDXCNT_VOL_MUL(v))
#define soundSetPan(id, p) (REG_SOUNDXCNT(id) = (REG_SOUNDXCNT(id) & ~(0x7Fu << 16)) | SOUNDXCNT_PAN(p))
#define soundSetFreq(id, f) (REG_SOUNDXTMR(id) = SOUNDXTMR_FREQ(f))
#define soundCaptureStart(buffer, words, unit, add, mixer, repeat, format) capture_start(buffer, words, unit)
#define soundCaptureStop(unit) ((unit) ? (REG_SNDCAP1CNT = 0) : (REG_SNDCAP0CNT = 0))
#define DC_FlushRange(p, n) ((void)0)
#else
#include "psx_runtime.h"
#endif

#define VOICES 24
#define RING 4096          /* samples per voice ring: ~93 ms at 44.1 kHz, longer at lower pitches */
#define LEAD 1             /* frames of samples written before a channel starts (plus a quarter frame) */
#define BUS_HZ 33513982u   /* (a channel reads a sample every 2 x its timer value bus cycles) */
#define RINGS 16

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
    s16 h1, h2;            /* its last two samples (gauss_like's) */
    int invert;            /* its louder side's volume is negative (the PS1 inverts the wave: written negated) */
    u64 read;              /* the channel's read position (samples since its start, x 65536), from the clock */
    u32 since;             /* clock() when read was last brought up to date */
    u16 timer;             /* the channel's timer value */
} Voice;

/* The time, in bus cycles / 64 (wrapping): timers 0 and 1 cascaded on the ARM7, which has them free (the clock IRQ
 * is timer 3). On the ARM9 its timers are the profiler's and the movies', so there a frame counts as one DS VBlank. */
#ifdef ARM7
static void clock_init(void) {
    TIMER_CR(0) = TIMER_CR(1) = 0;
    TIMER_DATA(0) = TIMER_DATA(1) = 0;
    TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(0) = TIMER_ENABLE | TIMER_DIV_64;
}
static u32 clock_now(void) {
    u16 hi = TIMER_DATA(1), lo = TIMER_DATA(0), hi2 = TIMER_DATA(1);
    if (hi2 != hi) lo = 0;
    return (u32)hi2 << 16 | lo;
}
#else
static u32 frame_clock;
static void clock_init(void) {}
static u32 clock_now(void) { return frame_clock; }
#endif

static Voice voices[VOICES];
/* Noise voices play on the PSG noise channels 14 and 15 (the DS has noise only there), the two loudest at once */
static struct { int voice, sound_id; u8 volume, pan; u16 freq; } noise_slots[2] = {{-1, -1, 0, 0, 0}, {-1, -1, 0, 0, 0}};
static s16 (*rings)[RING]; /* RINGS x RING, in PS1 RAM the port never uses (psx_spare_alloc) */
static u32 rings_used;     /* bit per ring */

static void ring_take(Voice *p) {
    if (p->ring) return;
    for (int i = 0; i < RINGS; ++i)
        if (!(rings_used >> i & 1)) { rings_used |= 1u << i; p->ring = rings[i]; return; }
}

static void ring_give_back(Voice *p) {
    if (!p->ring) return;
    rings_used &= ~(1u << ((p->ring - rings[0]) / RING));
    p->ring = NULL;
    p->write = p->flush_from = 0;
}
static u32 channel_used; /* bit per DS channel */
int port_hw_sound_stats[4];

static void key_on(unsigned v, void *user) {
    (void)user;
    Voice *p = &voices[v];
    p->write = 0;
    p->flush_from = 0;
    p->restart = 1;
    p->lost = 0;
    p->h1 = p->h2 = 0;
    ring_take(p);
    if (!p->ring) p->lost = 1; /* none free: it plays from later samples once it gets one */
}

/* The PS1 plays a voice through a 4-point Gaussian interpolation, which also filters it: on average over its phases
 * -3.3 dB at a quarter of the voice's sample rate, -11.8 dB at a half (docs/audio-validation.md). A DS channel plays
 * its samples as they are, so they get that filter first, as the three-tap [a, 1 - 2a, a] with a = 46/256 (-3.9 and
 * -11.1 dB there), a sample late. (What it can't do: the DS's playback still images above the voice's rate.) */
static void block(unsigned v, const int16_t *samples, void *user) {
    (void)user;
    Voice *p = &voices[v];
    if (!p->ring) return;
    int16_t out[28];
    int h1 = p->h1, h2 = p->h2;
    for (int i = 0; i < 28; ++i) {
        const int x = samples[i];
        int y = (46 * (h2 + x) + 164 * h1) >> 8;
        if (p->invert) y = -y;
        out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        h2 = h1; h1 = x;
    }
    p->h1 = (s16)h1; p->h2 = (s16)h2;
    unsigned at = p->write % RING;
    unsigned first = RING - at < 28 ? RING - at : 28;
    memcpy(p->ring + at, out, first * 2);
    if (first < 28) memcpy(p->ring, out + first, (28 - first) * 2);
    p->write += 28;
}

static const PsxSpuSink sink = {key_on, block, NULL};
static void echo_alloc(void);

#ifdef ARM7
static s16 *echo_buffer;
/* (the ARM9 allocated the rings and the echo's buffer: spu7.h) */
void port_hw_sound_init_arm7(int16_t *ring_memory, int16_t *echo_memory) {
    rings = (s16 (*)[RING])ring_memory;
    echo_buffer = echo_memory;
    echo_alloc();
#else
void port_hw_sound_init(void) {
    soundEnable();
    rings = psx_spare_alloc(RINGS * RING * sizeof(s16));
    echo_alloc(); /* (at start-up, as every spare PS1 RAM buffer: one that doesn't fit stops the port at boot) */
#endif
    for (int i = 0; i < VOICES; ++i)
        voices[i] = (Voice){NULL, 0, 0, -1, -1, 0, 64, 0, 0, 0};
    clock_init();
    psx_spu_set_hw(&sink);
}

/* Reverb, approximated (the DS has none): capture unit 0 records the left mix into a ring that channel 1 plays back
 * a delay later, and unit 1 the right mix through channel 3. The played-back mix is captured again, so each echo
 * repeats quieter, a decaying tail. Its level follows the reverb output volume and its delay the reverb work area's
 * size (rooms short, halls long); per-voice sends and the reverb's filters aren't modelled. */
#define ECHO_HZ 11025
#define ECHO_MAX 3072  /* samples per side: 0.28 s */
static struct { int on, ids[2]; u8 volume; unsigned words; s16 *ring; } echo = {0, {-1, -1}, 0, 0, NULL};
#ifdef ARM7
static void echo_alloc(void) { echo.ring = echo_buffer; }
#else
static void echo_alloc(void) { echo.ring = psx_spare_alloc(2 * ECHO_MAX * sizeof(s16)); }
#endif

static void echo_stop(void) {
    for (int k = 0; k < 2; ++k) {
        soundCaptureStop(k);
        if (echo.ids[k] >= 0) soundKill(echo.ids[k]);
        echo.ids[k] = -1;
    }
    echo.on = 0;
}

static void echo_frame(const PsxSpuReverbState *r) {
    if (!r->on) { if (echo.on) echo_stop(); return; }
    /* the work area holds about area / 4 stereo samples at 22.05 kHz */
    unsigned samples = (unsigned)((unsigned long long)r->area * ECHO_HZ / (4 * 22050));
    if (samples > ECHO_MAX) samples = ECHO_MAX;
    if (samples < 512) samples = 512;
    unsigned words = samples / 2;
    int level = (r->left < 0 ? -r->left : r->left) > (r->right < 0 ? -r->right : r->right)
                    ? (r->left < 0 ? -r->left : r->left) : (r->right < 0 ? -r->right : r->right);
    u8 volume = (u8)(level * 96 >> 15); /* at most 3/4: the tail must decay */
    if (!echo.on || words != echo.words) {
        echo_stop();
        memset(echo.ring, 0, 2 * ECHO_MAX * sizeof(s16));
        DC_FlushRange(echo.ring, 2 * ECHO_MAX * sizeof(s16));
        for (int k = 0; k < 2; ++k) {
            s16 *ring = echo.ring + k * ECHO_MAX;
            echo.ids[k] = soundPlaySampleChannel(1 + 2 * k, ring, SoundFormat_16Bit, words * 4, ECHO_HZ, volume,
                                                 k ? 127 : 0, true, 0);
            soundCaptureStart(ring, (u16)words, k, false, true, true, SoundCaptureFormat_16Bit);
        }
        echo.on = 1; echo.words = words; echo.volume = volume;
    } else if (volume != echo.volume) {
        for (int k = 0; k < 2; ++k) if (echo.ids[k] >= 0) soundSetVolume(echo.ids[k], volume);
        echo.volume = volume;
    }
}

/* a frequency's timer value, rounded as SOUNDXTMR_FREQ does it */
static u16 timer_of(unsigned freq) { return (u16)((BUS_HZ / 2 + (freq + 1) / 2) / freq); }

/* The channel's read position brought up to now, at its current timer value */
static void read_update(Voice *p, u32 now) {
    p->read += ((u64)(u32)(now - p->since) * 64 << 16) / (2u * p->timer);
    p->since = now;
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
#ifndef ARM7
    frame_clock += (u32)(BUS_HZ / 64 / 59.8261);
#endif
    const u32 now = clock_now();
    int candidates = 0;
    for (int v = 0; v < VOICES; ++v) {
        PsxSpuVoiceOutput *out = &outs[v];
        psx_spu_voice_output(v, out);
        int l = out->left < 0 ? -out->left : out->left, r = out->right < 0 ? -out->right : out->right;
        amps[v] = out->on && !out->noise ? (l > r ? l : r) + 1 : 0;
#ifdef PORT_SPU_SOLO /* (debug: only these voices heard) */
        if (!(PORT_SPU_SOLO >> v & 1)) amps[v] = 0;
#endif
        candidates += amps[v] > 0;
    }
    /* noise voices: the two loudest take channels 14 and 15 for this frame */
    int noise_voice[2] = {-1, -1}, noise_amp[2] = {0, 0};
    for (int v = 0; v < VOICES; ++v) {
        if (!outs[v].on || !outs[v].noise) continue;
        int l = outs[v].left < 0 ? -outs[v].left : outs[v].left, r = outs[v].right < 0 ? -outs[v].right : outs[v].right;
        int a = (l > r ? l : r) + 1;
        if (a > noise_amp[0]) { noise_amp[1] = noise_amp[0]; noise_voice[1] = noise_voice[0]; noise_amp[0] = a; noise_voice[0] = v; }
        else if (a > noise_amp[1]) { noise_amp[1] = a; noise_voice[1] = v; }
    }
    int reserved = (noise_voice[0] >= 0) + (noise_voice[1] >= 0);
    u32 reserved_mask = reserved ? (reserved == 2 ? 0xC000u : 0x4000u) : 0;
    /* reverb: channels 1 and 3 play the captured mix back (echo_frame) */
    PsxSpuReverbState reverb;
    psx_spu_reverb_state(&reverb);
    if (reverb.on) { reserved += 2; reserved_mask |= 0x000Au; }
    for (int v = 0; v < VOICES; ++v) /* a sample voice on a channel noise needs now gives it up */
        if (voices[v].channel >= 0 && (reserved_mask >> voices[v].channel & 1)) { stop(&voices[v]); voices[v].lost = 1; }
    const int slots = 16 - reserved;
    /* the slots-th largest amplitude: voices below it give their channel up */
    int cut = 1;
    if (candidates > slots) {
        int sorted[VOICES], n = 0;
        for (int v = 0; v < VOICES; ++v) if (amps[v]) sorted[n++] = amps[v];
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && sorted[j - 1] < sorted[j]; --j) { int t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t; }
        cut = sorted[slots - 1];
    }
    int sounding = 0, granted = 0;
    for (int v = 0; v < VOICES; ++v) {
        Voice *p = &voices[v];
        PsxSpuVoiceOutput *out = &outs[v];
        int keep = amps[v] && amps[v] >= cut && granted < slots;
        if (!keep) {
            if (amps[v]) port_hw_sound_stats[1]++;
            if (amps[v]) p->lost = 1; /* still sounding without a channel: its ring falls behind */
            stop(p);
            if (p->lost || !out->on) ring_give_back(p); /* it starts over, or it is silent for good */
            continue;
        }
        ++granted;
        int l = out->left < 0 ? -out->left : out->left, r = out->right < 0 ? -out->right : out->right;
        /* a negative volume inverts the voice on that side (pseudo-surround copies): one channel plays both sides,
         * so its samples follow the louder side's sign (from the next samples written) */
        p->invert = (l >= r ? out->left : out->right) < 0;
        int vol = (amps[v] - 1) * 127 / 0x4000;
        if (vol > 127) vol = 127;
        int pan = l + r ? 64 + 63 * (r - l) / (l + r) : 64;
        /* the rings are written once per emulated frame, one per DS VBlank (59.83 Hz) when the port keeps pace: the
         * channels read at that rate (else they gain 0.3% on their writes and catch them on a long note) */
        unsigned rate = 44100u * out->pitch / 4096;
        if (rate < 1) rate = 1;
        const unsigned frame = rate / 60; /* (samples a frame) */
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
        if (p->sound_id >= 0) {
            /* the samples come a frame's worth at a time, so the channel must stay more than a frame behind them and
             * less than a ring: its rate follows the voice's pitch, nudged (at most 2%) to keep it 1.5 frames behind
             * the writes (the DS's VBlank is 59.83 Hz, timer values round, the port may fall behind). Should it get
             * outside them anyway it starts over from the next samples: a gap, not a buzz of the ring's old contents
             * crossing the writes every frame. */
            read_update(p, now);
            const long long lead = (long long)p->write - (long long)(p->read >> 16);
            if (lead < (long long)frame || lead > RING - 2 * (long long)frame) {
                stop(p);
                p->write = p->flush_from = 0;
                port_hw_sound_stats[3]++;
                continue;
            }
            /* (the error over ~4 s, the VBlank's 59.83 Hz as it stands: frame-time jitter mustn't wobble the pitch) */
            long long want = (long long)rate * 59826 / 60000 + (lead - 3 * (long long)frame / 2) / 4;
            const long long most = rate / 50 + 1;
            if (want > (long long)rate + most) want = rate + most;
            if (want < (long long)rate - most) want = rate - most;
            rate = (unsigned)want;
        }
        const u16 freq = rate > 65535 ? 65535 : (u16)rate;
        if (p->sound_id < 0 && p->lost) {
            /* back from losing its channel: start over from the samples written from now on */
            ring_take(p);
            if (!p->ring) continue;
            p->lost = 0;
            p->write = p->flush_from = 0;
            continue;
        }
        if (p->sound_id < 0) {
            /* the samples arrive a frame's worth at a time (the advances, then this update): with less than that
             * written, as for a note keyed on partway through the frame, the channel would read past them before
             * the next ones and play the ring's old contents, crossing the writes each frame (a 60 Hz buzz) */
            if (p->write < LEAD * frame + frame / 4 + 1) continue;
            int ch = -1;
            for (int c = 0; c < 16; ++c) if (!((channel_used | reserved_mask) & (1u << c))) { ch = c; break; }
            if (ch < 0) continue;
            p->channel = ch;
            channel_used |= 1u << ch;
            p->sound_id = soundPlaySampleChannel(ch, p->ring, SoundFormat_16Bit, RING * 2, freq, (u8)vol, (u8)pan,
                                                 true, 0);
            p->volume = (u8)vol; p->pan = (u8)pan; p->freq = freq;
            p->timer = timer_of(freq); p->read = 0; p->since = now;
            port_hw_sound_stats[0]++;
        } else {
            if (vol != p->volume) { soundSetVolume(p->sound_id, (u8)vol); p->volume = (u8)vol; }
            if (pan != p->pan) { soundSetPan(p->sound_id, (u8)pan); p->pan = (u8)pan; }
            if (freq != p->freq) {
                soundSetFreq(p->sound_id, freq);
                p->freq = freq; p->timer = timer_of(freq);
            }
        }
        ++sounding;
    }
    unsigned hz = psx_spu_noise_hz();
    u16 noise_freq = hz > 65535 ? 65535 : hz < 1 ? 1 : (u16)hz;
    for (int k = 0; k < 2; ++k) {
        int v = noise_voice[k];
        if (noise_slots[k].voice != v && noise_slots[k].sound_id >= 0) {
            soundKill(noise_slots[k].sound_id);
            noise_slots[k].sound_id = -1;
        }
        noise_slots[k].voice = v;
        if (v < 0) continue;
        const PsxSpuVoiceOutput *out = &outs[v];
        int l = out->left < 0 ? -out->left : out->left, r = out->right < 0 ? -out->right : out->right;
        int vol = (noise_amp[k] - 1) * 127 / 0x4000;
        if (vol > 127) vol = 127;
        u8 pan = (u8)(l + r ? 64 + 63 * (r - l) / (l + r) : 64);
        if (noise_slots[k].sound_id < 0) {
            noise_slots[k].sound_id = soundPlayNoiseChannel(14 + k, noise_freq, (u8)vol, pan);
            noise_slots[k].volume = (u8)vol; noise_slots[k].pan = pan; noise_slots[k].freq = noise_freq;
        } else {
            if (vol != noise_slots[k].volume) { soundSetVolume(noise_slots[k].sound_id, (u8)vol); noise_slots[k].volume = (u8)vol; }
            if (pan != noise_slots[k].pan) { soundSetPan(noise_slots[k].sound_id, pan); noise_slots[k].pan = pan; }
            if (noise_freq != noise_slots[k].freq) { soundSetFreq(noise_slots[k].sound_id, noise_freq); noise_slots[k].freq = noise_freq; }
        }
        ++sounding;
    }
    echo_frame(&reverb);
    port_hw_sound_stats[2] = sounding;
}
#endif
