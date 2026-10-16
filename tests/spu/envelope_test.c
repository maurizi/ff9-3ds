#include <assert.h>
#include <stdio.h>
#include "psx_spu_envelope.h"
#include "envelope_cases.h"
#include "envelope_reference.h"

int main(void) {
    for (unsigned rate = 0; rate < 128; ++rate) {
        uint32_t hash = 2166136261u;
        for (unsigned mask_index = 0; mask_index < 2; ++mask_index) {
            unsigned mask = mask_index ? 0x7c : 0x7f;
            for (unsigned mode = 0; mode < 8; ++mode) {
                for (unsigned initial = 0; initial < 7; ++initial) {
                    PsxSpuEnvelope env;
                    psx_spu_envelope_configure(&env, rate, mask, mode & 1, mode & 2, mode & 4);
                    int16_t level = envelope_initial_levels[initial];
                    for (unsigned i = 0; i < envelope_case_ticks(rate); ++i) {
                        int active = psx_spu_envelope_tick(&env, &level);
                        hash = envelope_hash(hash, (uint16_t)level);
                        hash = envelope_hash(hash, env.counter);
                        hash = envelope_hash(hash, active);
                    }
                }
            }
        }
        if (hash != envelope_reference_hashes[rate]) {
            printf("Envelope rate %02x: %08lx != %08lx\n", rate,
                   (unsigned long)hash, (unsigned long)envelope_reference_hashes[rate]);
            return 1;
        }
    }

    /* Hand-calculated fastest linear attack/exponential decay. ADSR sustain
     * rate 127 holds; release rate 0 finishes on its first sample. */
    PsxSpuAdsr adsr = {0};
    psx_spu_adsr_set_registers(&adsr, 0, 0x1fc0);
    psx_spu_adsr_key_on(&adsr);
    static const int16_t expected[] = {14336, 28672, 32767, 16383, 8191, 4095, 2047};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; ++i) {
        psx_spu_adsr_tick(&adsr);
        assert(adsr.level == expected[i]);
        assert(adsr.phase == (i < 2 ? PSX_SPU_ADSR_ATTACK :
                             i < 6 ? PSX_SPU_ADSR_DECAY : PSX_SPU_ADSR_SUSTAIN));
    }
    for (unsigned i = 0; i < 40000; ++i) psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 2047 && adsr.phase == PSX_SPU_ADSR_SUSTAIN);
    psx_spu_adsr_key_off(&adsr);
    assert(adsr.phase == PSX_SPU_ADSR_RELEASE && adsr.level == 2047);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.phase == PSX_SPU_ADSR_OFF && adsr.level == 0);

    /* Key-off during attack preserves level. Repeated key-off does not reset
     * the release counter. An explicit ADSR write does reset that counter. */
    psx_spu_adsr_set_registers(&adsr, 0x3000, 12);
    psx_spu_adsr_key_on(&adsr);
    for (unsigned i = 0; i < 4; ++i) psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 14);
    psx_spu_adsr_key_off(&adsr);
    assert(adsr.level == 14 && adsr.envelope.counter == 0);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 14 && adsr.envelope.counter == 0x4000);
    psx_spu_adsr_key_off(&adsr);
    assert(adsr.envelope.counter == 0x4000);
    psx_spu_adsr_set_registers(&adsr, 0x3000, 12);
    assert(adsr.envelope.counter == 0);
    psx_spu_adsr_tick(&adsr);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 6);
    psx_spu_adsr_tick(&adsr);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.phase == PSX_SPU_ADSR_OFF && adsr.level == 0);

    /* Key-on retriggers from zero, force-off mutes immediately, and an off
     * voice ignores key-off. Release all-ones holds an existing level. */
    psx_spu_adsr_set_registers(&adsr, 0, 31);
    psx_spu_adsr_key_on(&adsr);
    psx_spu_adsr_tick(&adsr);
    psx_spu_adsr_key_off(&adsr);
    for (unsigned i = 0; i < 40000; ++i) psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 14336 && adsr.phase == PSX_SPU_ADSR_RELEASE);
    psx_spu_adsr_key_on(&adsr);
    assert(adsr.level == 0 && adsr.phase == PSX_SPU_ADSR_ATTACK);
    psx_spu_adsr_force_off(&adsr);
    psx_spu_adsr_key_off(&adsr);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.phase == PSX_SPU_ADSR_OFF && adsr.level == 0);

    /* Already-off ENVX writes survive an end/mute action during IRQ fetching. */
    adsr.level = 1234;
    psx_spu_adsr_force_off(&adsr);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 1234 && adsr.phase == PSX_SPU_ADSR_OFF);

    /* Both exponential sustain modes and full-scale sustain-level decoding. */
    psx_spu_adsr_set_registers(&adsr, 15, 0xc000);
    psx_spu_adsr_key_on(&adsr);
    for (unsigned i = 0; i < 4; ++i) psx_spu_adsr_tick(&adsr);
    assert(adsr.phase == PSX_SPU_ADSR_SUSTAIN && adsr.level == 16383);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 8191);
    psx_spu_adsr_set_registers(&adsr, 15, 0x8000);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 22527);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 32767);
    psx_spu_adsr_tick(&adsr);
    assert(adsr.level == 32767 && adsr.phase == PSX_SPU_ADSR_SUSTAIN);

    /* Direct volumes decode all signed 15-bit values; sweep writes retain the
     * current level. A saturated sweep stops, and equal writes restart it. */
    PsxSpuVolume volume = {0};
    for (unsigned raw = 0; raw < 0x8000; ++raw) {
        psx_spu_volume_write(&volume, raw);
        int expected_level = raw < 0x4000 ? (int)raw * 2 : (int)raw * 2 - 65536;
        assert(volume.level == expected_level && !volume.active);
        psx_spu_volume_tick(&volume);
        assert(volume.level == expected_level);
    }
    psx_spu_volume_write(&volume, 0);
    psx_spu_volume_write(&volume, 0x8000);
    assert(volume.level == 0 && volume.active);
    for (unsigned i = 0; i < 3; ++i) psx_spu_volume_tick(&volume);
    assert(volume.level == 32767 && !volume.active);
    psx_spu_volume_tick(&volume);
    assert(volume.level == 32767 && !volume.active);
    psx_spu_volume_write(&volume, 0x8000);
    assert(volume.level == 32767 && volume.active);
    psx_spu_volume_tick(&volume);
    assert(!volume.active);
    psx_spu_volume_write(&volume, 0x4000); /* direct -32768 */
    psx_spu_volume_write(&volume, 0x807f); /* paused sweep retains negative level */
    assert(volume.level == -32768 && !volume.active);
    psx_spu_volume_tick(&volume);
    assert(volume.level == -32768);
    psx_spu_volume_write(&volume, 0);
    psx_spu_volume_write(&volume, 0x9000); /* negative phase linear increase */
    psx_spu_volume_tick(&volume);
    assert(volume.level == -16384);
    psx_spu_volume_tick(&volume);
    assert(volume.level == -32768 && !volume.active);
    psx_spu_volume_write(&volume, 0x2000); /* 16384 */
    psx_spu_volume_write(&volume, 0xe000); /* exponential decrease */
    psx_spu_volume_tick(&volume);
    assert(volume.level == 8192);
    psx_spu_volume_write(&volume, 0x8030);
    psx_spu_volume_tick(&volume);
    assert(volume.envelope.counter == 0x4000);
    psx_spu_volume_write(&volume, 0x8030);
    assert(volume.envelope.counter == 0 && volume.level == 8192);

    puts("SPU envelopes: all rates/modes, ADSR, volume registers PASS");
    return 0;
}
