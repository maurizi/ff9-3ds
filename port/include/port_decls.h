/* Declarations the port adds to the decomp's (which now declare every call themselves: decomp request F31).
 */
#ifndef PORT_DECLS_H
#define PORT_DECLS_H

/* ---- hand-written: calls that don't fit any single prototype -------------------------------------------- */

/* Preserve the explicit AKAO hardware boundary. Without noipa, GCC embeds
 * original register stores inside akao_tick before link-time weakening, so
 * replacing the standalone symbols cannot intercept those stores. */
#ifdef PORT_BIN_main
void akao_keyOn(u32 x) __attribute__((noipa));
void akao_keyOff(s32 x) __attribute__((noipa));
void akao_setReverbVoices(u32 x) __attribute__((noipa));
void akao_setNoiseVoices(u32 x) __attribute__((noipa));
void akao_setPitchModVoices(u32 x) __attribute__((noipa));
void akao_setVoiceVolume(s32 v, u32 l, u32 r, s32 vol) __attribute__((noipa));
void akao_setVoicePitch(s32 v, s32 x) __attribute__((noipa));
void akao_setVoiceStartAddr(s32 v, s32 x) __attribute__((noipa));
void akao_setVoiceLoopAddr(s32 v, s32 x) __attribute__((noipa));
void akao_setVoiceAdsr1(s32 v, u16 x) __attribute__((noipa));
void akao_setVoiceAdsr2(s32 v, u16 x) __attribute__((noipa));
void akao_setVoiceAttack(s32 v, s32 a, s32 b) __attribute__((noipa));
void akao_setVoiceDecay(s32 v, s32 x) __attribute__((noipa));
void akao_setVoiceSustainLevel(s32 v, s32 x) __attribute__((noipa));
void akao_setVoiceSustain(s32 v, s32 a, s32 b) __attribute__((noipa));
void akao_setVoiceRelease(s32 v, s32 a, s32 b) __attribute__((noipa));
void akao_writeVoice(s32 v, AkaoVoiceRegs *p, s32 vol) __attribute__((noipa));
void akao_applyCdVolume(void) __attribute__((noipa));
void akao_initState(void) __attribute__((noipa));
#endif

#endif
