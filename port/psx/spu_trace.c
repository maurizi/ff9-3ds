#include "psx_spu_trace.h"
#ifdef __linux__
static PsxSpuTrace observer;
void psx_spu_set_trace(PsxSpuTrace trace) { observer = trace; }
void psx_spu_trace(unsigned type, unsigned a, unsigned b, const void *data) {
    if (observer) observer(type, a, b, data);
}
#endif
