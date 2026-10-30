#ifndef PSX_SPU_VOICE_H
#define PSX_SPU_VOICE_H
#include "psx_spu_envelope.h"
#include "psx_spu_source.h"

enum { PSX_SPU_VOICES = 24 };
typedef struct {
    uint16_t registers[8];
    PsxSpuSource source;
    PsxSpuAdsr adsr;
    PsxSpuVolume volume[2];
    int32_t last_sample; /* after ADSR, before stereo volumes/pitch modulation */
} PsxSpuVoice;

typedef struct {
    int32_t dry[2], reverb_send[2];
    int16_t capture[2]; /* voices 1 and 3, after ADSR */
    uint32_t ended; /* bits to OR into ENDX */
} PsxSpuVoiceMix;

/* register_index is 0..7 (the 16-byte voice-register block). Writes and key
 * events are explicit, including equal-value writes. Event scheduling and the
 * global SPU registers belong to the enclosing core. Initialize voices to zero. */
void psx_spu_voice_write(PsxSpuVoice *voice, unsigned register_index, uint16_t value);
uint16_t psx_spu_voice_read(const PsxSpuVoice *voice, unsigned register_index);
void psx_spu_voice_key_on(PsxSpuVoice *voice);
void psx_spu_voice_key_off(PsxSpuVoice *voice);

/* Generate one sample for all 24 voices, preserving precision in the sums.
 * The caller supplies this sample's shared noise value and enables fetching of
 * off voices when servicing SPU RAM IRQs. Reverb, CD/external inputs, global
 * mute, clipping/main volume and output are later stages, not approximated here. */
void psx_spu_mix_voices(PsxSpuVoice voices[PSX_SPU_VOICES], const uint8_t *ram,
                         int16_t noise, uint32_t noise_mask, uint32_t modulation_mask,
                         uint32_t reverb_mask, int fetch_off_voices, PsxSpuVoiceMix *mix);

/* psx_spu_mix_voices for n <= PSX_SPU_BLOCK_SPAN consecutive samples with no key events or register writes
 * between them; noise[s] is sample s's shared noise value. Identical results, voice by voice. */
enum { PSX_SPU_BLOCK_SPAN = 64 };
typedef struct {
    int32_t dry[PSX_SPU_BLOCK_SPAN][2], reverb_send[PSX_SPU_BLOCK_SPAN][2];
    int16_t capture[PSX_SPU_BLOCK_SPAN][2];
    uint32_t ended;
} PsxSpuVoiceBlock;
/* The hardware-mixing path (design §7.0): advance a voice's state over n samples exactly as mixing would (ADSR
 * per sample, source position, block crossings, loop/end flags) without producing samples. Each block the voice
 * decodes, in play order, goes to block(voice, samples, user) (28 samples), or is decoded where target (if any)
 * returns. Pitch modulation and noise use the
 * plain pitch here. */
void psx_spu_step_voice(PsxSpuVoice *voice, unsigned index, const uint8_t *ram, unsigned n, int fetch_off,
                        uint32_t *ended, void (*block)(unsigned voice, const int16_t *samples, void *user),
                        int16_t *(*target)(unsigned voice, int *negate, void *user), void *user);
void psx_spu_mix_voices_block(PsxSpuVoice voices[PSX_SPU_VOICES], const uint8_t *ram, unsigned n,
                              const int16_t *noise, uint32_t noise_mask, uint32_t modulation_mask,
                              uint32_t reverb_mask, int fetch_off_voices, PsxSpuVoiceBlock *out);
#endif
