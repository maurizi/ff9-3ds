/* Register/timing integration for the shared SPU voice stage. Streamed
 * CD input and IRQ delivery remain unfinished; see audio-validation.md. */
#include "psx_spu.h"
#include "spu_hot.h"
#include "psx_spu_voice.h"
#include "psx_spu_reverb.h"
#include "psx_spu_trace.h"
#include "platform.h"
#include "profile.h"
#include <string.h>

extern unsigned short psx_spu_regs[0x200];
extern unsigned char *psx_spu_ram;
static PsxSpuVoice voices[PSX_SPU_VOICES];
static PsxSpuVolume main_volume[2];
static PsxSpuNoise noise;
static PsxSpuReverb reverb;
static uint32_t key_on, key_off, ended;
static unsigned sample_fraction, capture_offset;
static int rendering;

static SPU_HOT int16_t clamp16(int32_t value) {
    return value > 32767 ? 32767 : value < -32768 ? -32768 : value;
}

static SPU_HOT uint32_t flags(unsigned offset) {
    return psx_spu_regs[offset / 2] | ((uint32_t)psx_spu_regs[offset / 2 + 1] << 16);
}

SPU_HOT void psx_spu_set_rendering(int enabled) { rendering = !!enabled; }

SPU_HOT void psx_spu_reset(void) {
    psx_spu_trace(PSX_SPU_TRACE_RESET, 0, 0, NULL);
    memset(voices, 0, sizeof voices);
    memset(main_volume, 0, sizeof main_volume);
    memset(psx_spu_regs, 0, 0x400);
    key_on = key_off = ended = sample_fraction = capture_offset = 0;
    noise = (PsxSpuNoise){0, 1};
    psx_spu_reverb_reset(&reverb);
    psx_spu_write(0x1aa, 0xc000); /* libspu initializes enabled/unmuted */
}

SPU_HOT uint16_t psx_spu_read(unsigned offset) {
    if ((offset & 1) || offset >= 0x400) return 0xffff;
    if (offset < 0x180) return psx_spu_voice_read(&voices[offset / 16], (offset & 15) / 2);
    if (offset == 0x19c) return ended;
    if (offset == 0x19e) return ended >> 16;
    if (offset == 0x1b8 || offset == 0x1ba) return main_volume[(offset - 0x1b8) / 2].level;
    if (offset >= 0x200 && offset < 0x260)
        return voices[(offset - 0x200) / 4].volume[(offset & 2) / 2].level;
    return psx_spu_regs[offset / 2];
}

SPU_HOT void psx_spu_voice_register(unsigned voice, unsigned offset, uint16_t value) {
    if (voice < PSX_SPU_VOICES && offset < 16) psx_spu_write(voice * 16 + offset, value);
}

SPU_HOT void psx_spu_write(unsigned offset, uint16_t value) {
    if ((offset & 1) || offset >= 0x400) return;
    if (offset == 0x19c || offset == 0x19e || offset == 0x1ae) return;
    psx_spu_trace(PSX_SPU_TRACE_WRITE, offset, value, NULL);
    uint16_t old = psx_spu_regs[offset / 2];
    psx_spu_regs[offset / 2] = value;
    if (offset < 0x180) {
        psx_spu_voice_write(&voices[offset / 16], (offset & 15) / 2, value);
    } else if (offset == 0x180 || offset == 0x182) {
        psx_spu_volume_write(&main_volume[(offset - 0x180) / 2], value);
    } else if (offset >= 0x188 && offset <= 0x18e) {
        uint32_t *pending = offset < 0x18c ? &key_on : &key_off;
        unsigned shift = (offset & 2) * 8;
        *pending = ((*pending & ~(0xffffu << shift)) | ((uint32_t)value << shift)) & 0xffffff;
    } else if (offset == 0x1a2) {
        psx_spu_reverb_base(&reverb, value);
    } else if (offset >= 0x1c0 && offset < 0x200) {
        reverb.registers[(offset - 0x1c0) / 2] = value;
    } else if (offset == 0x1aa) {
        if ((old & 0x8000) && !(value & 0x8000))
            for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) psx_spu_adsr_force_off(&voices[i].adsr);
        /* Mode readback only. DMA/IRQ status flags still require implementation. */
        psx_spu_regs[0x1ae / 2] = (psx_spu_regs[0x1ae / 2] & ~0x3f) | (value & 0x3f);
    } else if (offset == 0x1b8 || offset == 0x1ba) {
        main_volume[(offset - 0x1b8) / 2].level = (int16_t)value;
    }
}

static SPU_HOT void sync_readbacks(void) {
    for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) {
        psx_spu_regs[i * 8 + 6] = psx_spu_voice_read(&voices[i], 6);
        psx_spu_regs[i * 8 + 7] = psx_spu_voice_read(&voices[i], 7);
        psx_spu_regs[0x100 + i * 2] = voices[i].volume[0].level;
        psx_spu_regs[0x101 + i * 2] = voices[i].volume[1].level;
    }
    psx_spu_regs[0x19c / 2] = ended;
    psx_spu_regs[0x19e / 2] = ended >> 16;
    psx_spu_regs[0x1b8 / 2] = main_volume[0].level;
    psx_spu_regs[0x1ba / 2] = main_volume[1].level;
}

/* Everything after voice mixing for one output sample: capture buffers, reverb, main volume, output. */
static SPU_HOT void finish_sample(const int32_t dry_in[2], const int32_t send[2], const int16_t capture[2],
                                  int16_t output[2]) {
    uint16_t control = psx_spu_regs[0x1aa / 2];
    /* Capture writes precede the reverb RAM accesses, including when software
     * programs an unusual ESA/offset that overlaps a capture buffer. */
    for (unsigned channel = 0; channel < 2; ++channel) {
        /* PCM capture buffers: CD input is currently absent; voice capture is
         * post-ADSR and independent of global output mute. All addresses fit. */
        /* Even offsets into the little-endian RAM image: halfword stores. */
        uint16_t *ram = __builtin_assume_aligned(psx_spu_ram, 2);
        ram[((2 + channel) * 0x400 + capture_offset) / 2] = capture[channel];
        ram[(channel * 0x400 + capture_offset) / 2] = 0;
    }
    int16_t wet[2];
    psx_spu_reverb_tick(&reverb, psx_spu_ram,
        (control & 0x4000) ? clamp16(send[0]) : 0,
        (control & 0x4000) ? clamp16(send[1]) : 0, !!(control & 0x80), wet);
    for (unsigned channel = 0; channel < 2; ++channel) {
        int32_t dry = (control & 0x4000) ? dry_in[channel] : 0;
        int32_t mixed = dry + ((wet[channel] * (int16_t)psx_spu_regs[0xc2 + channel]) >> 15);
        output[channel] = clamp16((clamp16(mixed) * main_volume[channel].level) >> 15);
        if (main_volume[channel].active) psx_spu_volume_tick(&main_volume[channel]);
    }
#ifdef __linux__
    const int32_t stages[8] = {
        (control & 0x4000) ? dry_in[0] : 0, (control & 0x4000) ? dry_in[1] : 0,
        (control & 0x4000) ? clamp16(send[0]) : 0,
        (control & 0x4000) ? clamp16(send[1]) : 0,
        wet[0], wet[1], output[0], output[1]
    };
    psx_spu_trace(PSX_SPU_TRACE_SAMPLE, 0, 0, stages);
#endif
    capture_offset = (capture_offset + 2) & 0x3ff;
    psx_spu_regs[0x1ae / 2] = (psx_spu_regs[0x1ae / 2] & ~0x800) | (capture_offset & 0x200 ? 0x800 : 0);
}

static SPU_HOT void sample_frame(int16_t output[2]) {
    uint16_t control = psx_spu_regs[0x1aa / 2];
    PsxSpuVoiceMix mix;
    psx_spu_mix_voices(voices, psx_spu_ram, (int16_t)noise.level,
                       flags(0x194), flags(0x190), flags(0x198), !!(control & 0x40), &mix);
    ended |= mix.ended;
    psx_spu_noise_tick(&noise, (control >> 8) & 63);
    finish_sample(mix.dry, mix.reverb_send, mix.capture, output);
    /* Key events take effect after one output sample. Off then on means a
     * simultaneous key-on retriggers the voice. Repeated writes remain events. */
    if (key_on || key_off) {
        for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) {
            if (key_off & (1u << i)) psx_spu_voice_key_off(&voices[i]);
            if (key_on & (1u << i)) {
                psx_spu_voice_key_on(&voices[i]);
                ended &= ~(1u << i);
            }
        }
        key_on = key_off = 0;
    }
}

/* A span can be mixed voice by voice only if no voice can fetch SPU RAM that this span's capture or reverb
 * writes would change first: then the order of reads and writes doesn't matter. Each voice reads at most
 * `blocks` 16-byte blocks from its current address and from its loop address (loops land there, and loop
 * starts it records lie inside the first range). Unusual layouts fall back to sample by sample. */
static SPU_HOT int span_independent(unsigned n) {
    uint16_t control = psx_spu_regs[0x1aa / 2];
    uint32_t reverb_from = (control & 0x80) ? reverb.base * 2 : 0x80000; /* bytes; writes only when enabled */
    if (reverb_from < 0x1000) return 0;
    /* The CD input buffers (0..0x7ff) only ever receive zeros here. If the bytes this span will write are
     * already zero, those writes change nothing and voices may read that area too (idle voices often loop
     * at address 0). The voice 1/3 capture buffers (0x800..0xfff) do change. */
    uint32_t capture_from = 0x800;
    const uint16_t *ram = __builtin_assume_aligned(psx_spu_ram, 2);
    for (unsigned k = 0; k < n && capture_from; ++k) {
        unsigned at = ((capture_offset + k * 2) & 0x3ff) / 2;
        if (ram[at] | ram[0x200 + at]) capture_from = 0;
    }
    unsigned blocks = (n * 0x3fffu + (PSX_SPU_BLOCK_SAMPLES << 12)) / (PSX_SPU_BLOCK_SAMPLES << 12) + 2;
    int fetch_off = !!(control & 0x40);
    for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) {
        const PsxSpuVoice *v = &voices[i];
        if (v->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off) continue;
        uint32_t starts[2] = {(uint32_t)v->source.address * 8, (uint32_t)(v->registers[7] & 0xfffe) * 8};
        for (unsigned k = 0; k < 2; ++k) {
            uint32_t from = starts[k], to = from + blocks * PSX_SPU_BLOCK_BYTES;
            if ((to > capture_from && from < 0x1000) || to > reverb_from) return 0;
        }
    }
    return 1;
}

static SPU_HOT void span_frames(int16_t *output, unsigned n) {
    uint16_t control = psx_spu_regs[0x1aa / 2];
    int16_t noise_levels[PSX_SPU_BLOCK_SPAN];
    for (unsigned s = 0; s < n; ++s) {
        noise_levels[s] = (int16_t)noise.level;
        psx_spu_noise_tick(&noise, (control >> 8) & 63);
    }
    static PsxSpuVoiceBlock block;
    psx_spu_mix_voices_block(voices, psx_spu_ram, n, noise_levels, flags(0x194), flags(0x190), flags(0x198),
                             !!(control & 0x40), &block);
    ended |= block.ended;
    for (unsigned s = 0; s < n; ++s)
        finish_sample(block.dry[s], block.reverb_send[s], block.capture[s], output + s * 2);
}

SPU_HOT void psx_spu_advance(unsigned ticks) {
    if (!rendering || !psx_spu_ram) return;
    unsigned profile_spu = port_profile_begin();
    /* Break down before adding, so even UINT_MAX ticks cannot overflow. */
    unsigned frames = ticks / 96;
    sample_fraction += ticks % 96;
    frames += sample_fraction / 96;
    sample_fraction %= 96;
    int16_t output[256 * 2];
    while (frames) {
        unsigned count = frames > 256 ? 256 : frames;
        unsigned i = 0;
        if (key_on || key_off) sample_frame(output), i = 1; /* key events apply after this sample */
        while (i < count) {
            unsigned n = count - i > PSX_SPU_BLOCK_SPAN ? PSX_SPU_BLOCK_SPAN : count - i;
            if (span_independent(n)) span_frames(output + i * 2, n);
            else for (unsigned k = 0; k < n; ++k) sample_frame(output + (i + k) * 2);
            i += n;
        }
        unsigned profile_sink = port_profile_begin();
        plat_audio_samples(output, count);
        port_profile_detail_end(PORT_PROFILE_AUDIO_SINK, profile_sink);
        frames -= count;
    }
    sync_readbacks();
    port_profile_detail_end(PORT_PROFILE_SPU, profile_spu);
}
