/* Optional diagnostics. Timer 2/3 form a free-running bus/64 counter; ordinary
 * builds do not touch the timers or insert timing calls. Categories overlap
 * (effect callbacks can invoke GTE/native exports), so do not sum their shares. */
#include "profile.h"
#ifdef PORT_PROFILE
#include <nds.h>
volatile unsigned int port_profile_stats[16];
volatile unsigned int port_profile_draw_stats[8];
volatile unsigned int port_profile_detail_stats[2 * PORT_PROFILE_DETAIL_CATEGORIES + 2];
static unsigned int ticks[PORT_PROFILE_CATEGORIES],calls[PORT_PROFILE_CATEGORIES];
static unsigned int draw_ticks[3],draw_calls[3];
static unsigned int detail_ticks[PORT_PROFILE_DETAIL_CATEGORIES],detail_calls[PORT_PROFILE_DETAIL_CATEGORIES];
static unsigned int elapsed,last_clock,samples,last_mode=~0u;
static unsigned int clock_now(void) {
    unsigned int high,low;
    do {high=TIMER_DATA(3);low=TIMER_DATA(2);} while(high!=TIMER_DATA(3));
    return (high<<16)|low;
}
unsigned int port_profile_wait_begin(void) { return clock_now(); }
void port_profile_wait_end(unsigned int start) {
    ticks[PORT_PROFILE_WAIT]+=clock_now()-start;calls[PORT_PROFILE_WAIT]++;
}
#ifndef PORT_PROFILE_FRAME_ONLY
unsigned int port_profile_begin(void) { return clock_now(); }
void port_profile_end(unsigned int category,unsigned int start) {
    ticks[category]+=clock_now()-start;calls[category]++;
}
void port_profile_draw_end(unsigned int kind,unsigned int start) {
    draw_ticks[kind]+=clock_now()-start;draw_calls[kind]++;
}
void port_profile_detail_end(unsigned int category,unsigned int start) {
    detail_ticks[category]+=clock_now()-start;detail_calls[category]++;
}
#endif
void port_profile_init(void) {
    TIMER_CR(2)=0;TIMER_CR(3)=0;TIMER_DATA(2)=0;TIMER_DATA(3)=0;
    TIMER_CR(3)=TIMER_ENABLE|TIMER_CASCADE;
    TIMER_CR(2)=TIMER_ENABLE|TIMER_DIV_64;
    last_clock=clock_now();
}
void port_profile_frame(unsigned int mode,unsigned int frame) {
    unsigned int now=clock_now();
    if(mode!=last_mode) {elapsed=0;samples=0;last_mode=mode;}
    else {elapsed+=now-last_clock;samples++;}
    last_clock=now;
    if(samples==128) {
        port_profile_stats[0]=0; /* Publish validity after the complete block. */
        port_profile_draw_stats[0]=0;
        port_profile_detail_stats[0]=0;
        for(unsigned i=0;i<PORT_PROFILE_DETAIL_CATEGORIES;i++) {
            port_profile_detail_stats[1+i]=detail_ticks[i];
            port_profile_detail_stats[1+PORT_PROFILE_DETAIL_CATEGORIES+i]=detail_calls[i];
        }
        port_profile_detail_stats[2*PORT_PROFILE_DETAIL_CATEGORIES+1]=0x44545031;
        port_profile_detail_stats[0]=frame;
        for(unsigned i=0;i<3;i++) {
            port_profile_draw_stats[1+i]=draw_ticks[i];
            port_profile_draw_stats[4+i]=draw_calls[i];
        }
        port_profile_draw_stats[7]=0x44525731;port_profile_draw_stats[0]=frame;
        port_profile_stats[1]=BUS_CLOCK/64;port_profile_stats[2]=frame;
        port_profile_stats[3]=mode;port_profile_stats[4]=elapsed;
        for(unsigned i=0;i<PORT_PROFILE_CATEGORIES;i++) {
            port_profile_stats[5+i]=ticks[i];port_profile_stats[10+i]=calls[i];
        }
        port_profile_stats[15]=0x50524631;port_profile_stats[0]=samples;
        samples=0;elapsed=0;
    }
    if(!samples) {
        for(unsigned i=0;i<PORT_PROFILE_CATEGORIES;i++) ticks[i]=calls[i]=0;
        for(unsigned i=0;i<3;i++) draw_ticks[i]=draw_calls[i]=0;
        for(unsigned i=0;i<PORT_PROFILE_DETAIL_CATEGORIES;i++) detail_ticks[i]=detail_calls[i]=0;
    }
}
#endif
