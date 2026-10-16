#ifndef PSX_SPU_TRACE_H
#define PSX_SPU_TRACE_H
#include <stdint.h>
/* Host diagnostics, omitted entirely from ARM9 sample/register paths.
 * RESET: no payload; WRITE: a=byte offset,b=u16 value; RAM: a=byte address,
 * b=byte count,data=bytes; SAMPLE: data=eight int32 values (stereo dry, clamped
 * send, raw wet, final PCM). Events are ordered relative to SAMPLE records. */
enum { PSX_SPU_TRACE_RESET, PSX_SPU_TRACE_WRITE, PSX_SPU_TRACE_RAM, PSX_SPU_TRACE_SAMPLE };
#ifdef __linux__
typedef void (*PsxSpuTrace)(unsigned type, unsigned a, unsigned b, const void *data);
void psx_spu_set_trace(PsxSpuTrace trace);
void psx_spu_trace(unsigned type, unsigned a, unsigned b, const void *data);
#else
static inline void psx_spu_trace(unsigned type, unsigned a, unsigned b, const void *data) {
    (void)type; (void)a; (void)b; (void)data;
}
#endif
#endif
