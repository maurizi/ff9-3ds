/* AKAO's hardware-write boundary. Preserve the original arithmetic and register
 * order while delivering explicit writes to the sample-clocked SPU core.
 * See config/overrides.txt; the decomp checkout is never changed. */
#define M2CTX
#include "ff9.h"
#include "psx_spu.h"

static u16 voice_read(s32 voice, unsigned offset) {
    return (u32)voice < 24 ? psx_spu_read((u32)voice * 16 + offset) : 0;
}
static void flags_write(unsigned offset, u32 value) {
    psx_spu_write(offset, value);
    psx_spu_write(offset + 2, value >> 16);
}
void akao_keyOn(u32 x) { flags_write(0x188, x); }
void akao_keyOff(s32 x) { flags_write(0x18c, (u32)x); }
void akao_setReverbVoices(u32 x) { flags_write(0x198, x); }
void akao_setNoiseVoices(u32 x) { flags_write(0x194, x); }
void akao_setPitchModVoices(u32 x) { flags_write(0x190, x); }

void akao_setVoiceVolume(s32 v, u32 l, u32 r, s32 vol) {
    if (vol) { l *= vol; r *= vol; l >>= 7; r >>= 7; }
    psx_spu_voice_register(v, 0, l & 0x7fff);
    psx_spu_voice_register(v, 2, r & 0x7fff);
}
void akao_setVoicePitch(s32 v, s32 x) { psx_spu_voice_register(v, 4, x); }
void akao_setVoiceStartAddr(s32 v, s32 x) { psx_spu_voice_register(v, 6, (u32)x >> 3); }
void akao_setVoiceLoopAddr(s32 v, s32 x) { psx_spu_voice_register(v, 14, (u32)x >> 3); }
void akao_setVoiceAdsr1(s32 v, u16 x) { psx_spu_voice_register(v, 8, x); }
void akao_setVoiceAdsr2(s32 v, u16 x) { psx_spu_voice_register(v, 10, x); }
void akao_setVoiceAttack(s32 v, s32 a, s32 b) {
    psx_spu_voice_register(v, 8, (voice_read(v, 8) & 255) | (((u32)b >> 2) << 15) | ((u32)a << 8));
}
void akao_setVoiceDecay(s32 v, s32 x) {
    psx_spu_voice_register(v, 8, (voice_read(v, 8) & 0xff0f) | ((u32)x << 4));
}
void akao_setVoiceSustainLevel(s32 v, s32 x) {
    psx_spu_voice_register(v, 8, (voice_read(v, 8) & 0xfff0) | (u32)x);
}
void akao_setVoiceSustain(s32 v, s32 a, s32 b) {
    psx_spu_voice_register(v, 10, (voice_read(v, 10) & 0x3f) | (((u32)b >> 1) << 14) | ((u32)a << 6));
}
void akao_setVoiceRelease(s32 v, s32 a, s32 b) {
    psx_spu_voice_register(v, 10, (voice_read(v, 10) & 0xffc0) | (((u32)b >> 2) << 5) | (u32)a);
}
void akao_writeVoice(s32 v, AkaoVoiceRegs *p, s32 vol) {
    p->dirty = 0;
    s32 left = p->volL, right = p->volR;
    if (vol) {
        left = (s32)((u32)left * (u32)vol) >> 7;
        right = (s32)((u32)right * (u32)vol) >> 7;
    }
    psx_spu_voice_register(v, 0, left & 0x7fff);
    psx_spu_voice_register(v, 2, right & 0x7fff);
    psx_spu_voice_register(v, 4, p->pitch);
    psx_spu_voice_register(v, 6, p->startAddr >> 3);
    psx_spu_voice_register(v, 8, p->adsr1);
    psx_spu_voice_register(v, 10, p->adsr2);
    psx_spu_voice_register(v, 14, p->loopAddr >> 3);
}
void akao_applyCdVolume(void) {
    psx_spu_write(0x1b0, g_akaoCdVolumeHi);
    psx_spu_write(0x1b2, g_akaoCdVolumeHi);
}

/* AKAO initialization from its existing C equivalent, with hardware writes
 * routed through the same boundary as the per-tick voice updates. */
void akao_initState(void) {
    u16 i;
    AkaoChannel *c;

    g_akaoSubSeq = NULL;
    D_800809EC = g_akaoChannels;
    g_akaoSubChannels = NULL;
    D_8007F8D8[1] = 0;
    g_akaoSeq = &g_akaoMainSeq;
    D_8007F8D8[0] = 0;
    g_akaoLoadedBanks[5] = 0;
    g_akaoLoadedBanks[4] = 0;
    g_akaoLoadedBanks[3] = 0;
    g_akaoLoadedBanks[2] = 0;
    g_akaoLoadedBanks[1] = 0;
    g_akaoRandIndex = 0;
    g_akaoLoadedBanks[0] = 0;
    D_8007F8E0 = 1;
    g_akaoFlags.bankUpload = 0;
    g_akaoFlags.mode = 1;
    g_akaoSeState.activeMask = 0;
    g_akaoMainSeq.activeMask = 0;
    g_akaoMainSeq.priorityMask = 0;
    g_akaoMainSeq.musicId = 0;
    g_akaoSeState.pausedMask = 0;
    g_akaoMainSeq.pausedMask = 0;
    g_akaoSavedSeq.musicId = 0;
    g_akaoSavedSeq.activeMask = 0;
    g_akaoMainSeq.volume = 0x7F0000;
    g_akaoMainSeq.pan = 0x400000;
    g_akaoCdVolume = 0x7FFF0000;
    g_akaoPitchScaleLen = 0;
    g_akaoPitchScale = 0;
    g_akaoMainSeq.volumeFadeLen = 0;
    g_akaoMainSeq.panSlideLen = 0;
    g_akaoTempoScaleLen = 0;
    g_akaoTempoScale = 0;
    g_akaoCdVolumeFadeTicks = 0;
    psx_spu_write(0x180, 0x3FFF);
    psx_spu_write(0x182, 0x3FFF);
    psx_spu_write(0x1b0, 0x7FFF);
    psx_spu_write(0x1b2, 0x7FFF);
    g_akaoSkipMeasures = 0;
    g_akaoMuteMask = 0;
    g_akaoPauseFlags = 0;
    i = psx_spu_read(0x1aa);
    g_akaoSeState.noiseMask = 0;
    g_akaoMainSeq.noiseMask = 0;
    g_akaoSeState.reverbMask = 0;
    g_akaoMainSeq.reverbMask = 0;
    g_akaoSeState.pitchModMask = 0;
    g_akaoMainSeq.pitchModMask = 0;
    g_akaoMainSeq.ticksPerBeat = 0;
    g_akaoMainSeq.beat = 0;
    g_akaoMainSeq.beatsPerMeasure = 0;
    g_akaoMainSeq.measure = 0;
    g_akaoStream.volume = 0x7F00;
    g_akaoStream.volumeFadeLen = 0;
    g_akaoVoiceModeMasks[2] = 0;
    g_akaoVoiceModeMasks[1] = 0;
    g_akaoVoiceModeMasks[0] = 0;
    D_80080A20 = 0;
    psx_spu_write(0x1aa, (i & ~5) | 1);
    for (c = g_akaoChannels, i = 0; i < 32; i++, c++) {
        c->flags = 0;
        c->voice = 0x18;
        c->isSe = 0;
        c->age = 0;
    }
    c = g_akaoSeChannels;
    for (i = 8; i < 24;) {
        c->flags = 0;
        c->voice = i;
        i++;
        c->isSe = 1;
        c->age = 0;
        c->seVolume = 0x7F00;
        c->seVolumeSlideLen = 0;
        c->sePitchSlideLen = 0;
        c->sePitch = 0;
        c->noteVolumeSlideLen = 0;
        c++;
    }
    g_akaoSeq->keyOffMask = 0;
    g_akaoSeq->playingMask = 0;
    g_akaoSeq->keyOnMask = 0;
    g_akaoSeState.tempoAcc = 1;
    g_akaoSeState.tempo.w = 0x66A80000;
    g_akaoSeState.keyOffMask = 0;
    g_akaoSeState.playingMask = 0;
    g_akaoSeState.keyOnMask = 0;
    g_akaoSeq->reverbDepth = 0x3FFF000;
    g_akaoSeq->reverbDepthStep = 0;
    g_akaoSeq->reverbDepthSlideLen = 0;
    g_akaoFlags.dirty |= 0x80;
    akao_setReverbMode(4);
    SpuSetReverb(1);
    for (i = 0; i < 24; i++) {
        akao_setVoiceLoopAddr(i, 0x1030);
    }
    g_akaoVsyncs[1] = 0;
    g_akaoVsyncs[0] = 0;
    g_akaoTickPhase = 0;
}
