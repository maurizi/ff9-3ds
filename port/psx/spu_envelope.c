/* Sample-clocked envelope arithmetic; validation/scope in audio-validation.md. */
#include "psx_spu_envelope.h"
#include "spu_hot.h"
#include "spu_inline.h"

SPU_HOT void psx_spu_envelope_configure(PsxSpuEnvelope *env, uint8_t rate, uint8_t hold_mask,
                                int decrease, int exponential, int negative) {
    rate &= 0x7f;
    unsigned shift = rate >> 2;
    int step = 7 - (rate & 3);
    env->rate = rate;
    env->decrease = !!decrease;
    env->exponential = !!exponential;
    env->negative = !!negative && !(decrease && exponential);
    if ((!!decrease ^ !!negative) || (decrease && exponential)) step = -step - 1;
    if (shift < 11) step *= 1 << (11 - shift);
    env->step = step;
    env->counter = 0;
    env->increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    if (!env->increment && (rate & hold_mask) != hold_mask) env->increment = 1;
}

SPU_HOT int psx_spu_envelope_tick(PsxSpuEnvelope *env, int16_t *level) {
    return spu_envelope_tick(env, level);
}

SPU_HOT void psx_spu_volume_write(PsxSpuVolume *volume, uint16_t value) {
    if (!(value & 0x8000)) {
        int level = value & 0x7fff;
        if (level & 0x4000) level -= 0x8000;
        volume->level = level * 2;
        volume->active = 0;
        return;
    }
    psx_spu_envelope_configure(&volume->envelope, value & 0x7f, 0x7f,
                               value & 0x2000, value & 0x4000, value & 0x1000);
    volume->active = volume->envelope.increment != 0;
}

SPU_HOT void psx_spu_volume_tick(PsxSpuVolume *volume) {
    spu_volume_tick(volume);
}

static SPU_HOT void adsr_phase(PsxSpuAdsr *adsr, PsxSpuAdsrPhase phase) {
    unsigned rate = 0, mask = 0x7f;
    int decrease = 0, exponential = 0;
    adsr->phase = phase;
    adsr->target = 0;
    switch (phase) {
    case PSX_SPU_ADSR_ATTACK:
        rate = (adsr->adsr1 >> 8) & 0x7f;
        exponential = adsr->adsr1 >> 15;
        adsr->target = 32767;
        break;
    case PSX_SPU_ADSR_DECAY: {
        rate = ((adsr->adsr1 >> 4) & 15) * 4;
        mask = 0x7c;
        decrease = exponential = 1;
        unsigned target = ((adsr->adsr1 & 15) + 1) * 0x800;
        adsr->target = target > 32767 ? 32767 : target;
        break;
    }
    case PSX_SPU_ADSR_SUSTAIN:
        rate = (adsr->adsr2 >> 6) & 0x7f;
        decrease = (adsr->adsr2 >> 14) & 1;
        exponential = adsr->adsr2 >> 15;
        break;
    case PSX_SPU_ADSR_RELEASE:
        rate = (adsr->adsr2 & 31) * 4;
        mask = 0x7c;
        decrease = 1;
        exponential = (adsr->adsr2 >> 5) & 1;
        break;
    case PSX_SPU_ADSR_OFF:
        break;
    }
    psx_spu_envelope_configure(&adsr->envelope, rate, mask, decrease, exponential, 0);
}

SPU_HOT void psx_spu_adsr_set_registers(PsxSpuAdsr *adsr, uint16_t adsr1, uint16_t adsr2) {
    adsr->adsr1 = adsr1;
    adsr->adsr2 = adsr2;
    adsr_phase(adsr, adsr->phase);
}

SPU_HOT void psx_spu_adsr_key_on(PsxSpuAdsr *adsr) {
    adsr->level = 0;
    adsr_phase(adsr, PSX_SPU_ADSR_ATTACK);
}

SPU_HOT void psx_spu_adsr_key_off(PsxSpuAdsr *adsr) {
    if (adsr->phase != PSX_SPU_ADSR_OFF && adsr->phase != PSX_SPU_ADSR_RELEASE)
        adsr_phase(adsr, PSX_SPU_ADSR_RELEASE);
}

SPU_HOT void psx_spu_adsr_force_off(PsxSpuAdsr *adsr) {
    if (adsr->phase == PSX_SPU_ADSR_OFF) return;
    adsr->level = 0;
    adsr_phase(adsr, PSX_SPU_ADSR_OFF);
}

SPU_HOT void psx_spu_adsr_advance_phase(PsxSpuAdsr *adsr) {
    adsr_phase(adsr, adsr->phase == PSX_SPU_ADSR_RELEASE ? PSX_SPU_ADSR_OFF : adsr->phase + 1);
}

SPU_HOT void psx_spu_adsr_tick(PsxSpuAdsr *adsr) {
    spu_adsr_tick(adsr);
}
