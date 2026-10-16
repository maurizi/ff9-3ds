#ifndef PORT_PCM_RING_H
#define PORT_PCM_RING_H
#include <stdint.h>
enum { PORT_PCM_CAPACITY = 8192 };
/* Single ARM9 foreground producer and IRQ consumer. Publish cursors only after
 * copying; unsigned differences handle 32-bit wrap during multi-hour sessions.
 * Reset only while the consumer IRQ is stopped. No ARM7 shares this queue. */
typedef struct {
    volatile uint32_t written, read;
    volatile uint32_t underrun, callbacks;
    int16_t samples[PORT_PCM_CAPACITY * 2];
} PortPcmRing;
void port_pcm_reset(PortPcmRing *ring);
unsigned port_pcm_available(const PortPcmRing *ring);
unsigned port_pcm_push(PortPcmRing *ring, const int16_t *samples, unsigned frames);
/* Fill the complete request, using silence only for missing frames. Return the
 * number of queued frames consumed; never skip future data after starvation. */
unsigned port_pcm_pull(PortPcmRing *ring, int16_t *output, unsigned frames);
#endif
