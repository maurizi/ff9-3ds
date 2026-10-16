#ifndef PSX_SPU_ENVELOPE_H
#define PSX_SPU_ENVELOPE_H
#include <stdint.h>

/* One envelope step runs per 44.1 kHz output sample. Also used for volume sweeps. */
typedef struct {
    uint32_t counter, increment;
    int32_t step;
    uint8_t rate, decrease, exponential, negative;
} PsxSpuEnvelope;

void psx_spu_envelope_configure(PsxSpuEnvelope *env, uint8_t rate, uint8_t hold_mask,
                                int decrease, int exponential, int negative);
/* Updates *level; returns zero once the sweep has saturated. A held rate does
 * not tick or saturate, even if the current level is already at an endpoint. */
int psx_spu_envelope_tick(PsxSpuEnvelope *env, int16_t *level);

typedef struct {
    PsxSpuEnvelope envelope;
    int16_t level;
    uint8_t active;
} PsxSpuVolume;

/* VOLL/VOLR/MVOLL/MVOLR: direct signed 15-bit volume or a sweep starting from
 * the existing level. Explicit writes reset the sweep's rate counter. */
void psx_spu_volume_write(PsxSpuVolume *volume, uint16_t value);
void psx_spu_volume_tick(PsxSpuVolume *volume);

typedef enum {
    PSX_SPU_ADSR_OFF, PSX_SPU_ADSR_ATTACK, PSX_SPU_ADSR_DECAY,
    PSX_SPU_ADSR_SUSTAIN, PSX_SPU_ADSR_RELEASE
} PsxSpuAdsrPhase;

typedef struct {
    PsxSpuEnvelope envelope;
    uint16_t adsr1, adsr2;
    int16_t level, target;
    PsxSpuAdsrPhase phase;
} PsxSpuAdsr;

/* Call for each explicit ADSR register write, including repeated values. */
void psx_spu_adsr_set_registers(PsxSpuAdsr *adsr, uint16_t adsr1, uint16_t adsr2);
void psx_spu_adsr_key_on(PsxSpuAdsr *adsr);
void psx_spu_adsr_key_off(PsxSpuAdsr *adsr);
/* Stops an active envelope. Already-off voices retain manually written ENVX. */
void psx_spu_adsr_force_off(PsxSpuAdsr *adsr);
void psx_spu_adsr_tick(PsxSpuAdsr *adsr);
#endif
