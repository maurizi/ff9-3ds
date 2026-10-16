#ifndef PORT_DS_AUDIO_H
#define PORT_DS_AUDIO_H
#include <stdint.h>
/* Diagnostic snapshot: magic, state, produced, consumed, inserted silence,
 * callback count, queue high-water, producer waits, requested rate, format
 * failures, optional PCM hash/nonzero count. State 0=off,1=prefill,2=streaming,3=stopped,4=failed. */
extern volatile uint32_t port_audio_stats[12];
int port_audio_init(void);
void port_audio_stop(void);
unsigned port_audio_queued(void);
#endif
