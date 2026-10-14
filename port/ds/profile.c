/* Optional diagnostics. Timer 2/3 form a free-running bus/64 counter; ordinary
 * builds do not touch the timers or insert timing calls. Categories overlap
 * (effect callbacks can invoke GTE/native exports), so do not sum their shares. */
#include "profile.h"
#ifdef PORT_PROFILE
#include <nds.h>
volatile unsigned int port_profile_stats[16];
static unsigned int ticks[PORT_PROFILE_CATEGORIES],calls[PORT_PROFILE_CATEGORIES];
static unsigned int elapsed,last_clock,samples,last_mode=~0u;
unsigned int port_profile_begin(void) {
    unsigned int high,low;
    do {high=TIMER_DATA(3);low=TIMER_DATA(2);} while(high!=TIMER_DATA(3));
    return (high<<16)|low;
}
void port_profile_end(unsigned int category,unsigned int start) {
    ticks[category]+=port_profile_begin()-start;calls[category]++;
}
void port_profile_init(void) {
    TIMER_CR(2)=0;TIMER_CR(3)=0;TIMER_DATA(2)=0;TIMER_DATA(3)=0;
    TIMER_CR(3)=TIMER_ENABLE|TIMER_CASCADE;
    TIMER_CR(2)=TIMER_ENABLE|TIMER_DIV_64;
    last_clock=port_profile_begin();
}
void port_profile_frame(unsigned int mode,unsigned int frame) {
    unsigned int now=port_profile_begin();
    if(mode!=last_mode) {elapsed=0;samples=0;last_mode=mode;}
    else {elapsed+=now-last_clock;samples++;}
    last_clock=now;
    if(samples==128) {
        port_profile_stats[0]=0; /* Publish validity after the complete block. */
        port_profile_stats[1]=BUS_CLOCK/64;port_profile_stats[2]=frame;
        port_profile_stats[3]=mode;port_profile_stats[4]=elapsed;
        for(unsigned i=0;i<PORT_PROFILE_CATEGORIES;i++) {
            port_profile_stats[5+i]=ticks[i];port_profile_stats[10+i]=calls[i];
        }
        port_profile_stats[15]=0x50524631;port_profile_stats[0]=samples;
        samples=0;elapsed=0;
    }
    if(!samples) for(unsigned i=0;i<PORT_PROFILE_CATEGORIES;i++) ticks[i]=calls[i]=0;
}
#endif
