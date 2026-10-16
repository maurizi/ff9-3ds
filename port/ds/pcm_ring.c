#include "pcm_ring.h"
#include <string.h>
static void publish(void) { __asm__ volatile("" ::: "memory"); }
void port_pcm_reset(PortPcmRing *ring) {
    ring->written = ring->read = ring->underrun = ring->callbacks = 0;
}
unsigned port_pcm_available(const PortPcmRing *ring) {
    return ring->written - ring->read;
}
unsigned port_pcm_push(PortPcmRing *ring, const int16_t *samples, unsigned frames) {
    uint32_t written = ring->written, read = ring->read;
    publish();
    unsigned free = PORT_PCM_CAPACITY - (written - read);
    if (frames > free) frames = free;
    unsigned pos = written & (PORT_PCM_CAPACITY - 1);
    unsigned first = PORT_PCM_CAPACITY - pos;
    if (first > frames) first = frames;
    memcpy(ring->samples + pos * 2, samples, first * 4);
    if (frames > first) memcpy(ring->samples, samples + first * 2, (frames - first) * 4);
    publish();
    ring->written = written + frames;
    return frames;
}
unsigned port_pcm_pull(PortPcmRing *ring, int16_t *output, unsigned frames) {
    uint32_t read = ring->read, written = ring->written;
    publish();
    unsigned count = written - read;
    if (count > frames) count = frames;
    unsigned pos = read & (PORT_PCM_CAPACITY - 1);
    unsigned first = PORT_PCM_CAPACITY - pos;
    if (first > count) first = count;
    memcpy(output, ring->samples + pos * 2, first * 4);
    if (count > first) memcpy(output + first * 2, ring->samples, (count - first) * 4);
    if (frames > count) memset(output + count * 2, 0, (frames - count) * 4);
    publish();
    ring->read = read + count;
    ring->underrun += frames - count;
    ++ring->callbacks;
    return count;
}
