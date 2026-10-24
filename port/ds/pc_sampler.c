/* PROFILE=3 (DSi builds): a sampling profiler. Timer 1 interrupts 8000 times a second and counts the interrupted
 * instruction's address in 64-byte buckets; tools/ds_pc_profile.py reads the counts from a melonDS state and maps
 * them to functions. The BIOS's IRQ entry pushes {r0-r3, r12, lr} at the top of the IRQ stack: lr - 4 is the
 * interrupted instruction (approximately, in Thumb code), whatever the libnds dispatcher does next. */
#ifdef PORT_PC_SAMPLER
#include <nds.h>

#define MAIN_LO 0x02000000u
#define MAIN_BUCKETS ((0x022A0000u - MAIN_LO) >> 6)
#define ITCM_LO 0x01FF8000u
#define ITCM_BUCKETS (0x8000u >> 6)
/* header: magic, main base, main buckets, ITCM base, ITCM buckets, samples, outside (BIOS waits, other memory) */
volatile u32 port_pc_header[8] = {0x53504331, MAIN_LO, MAIN_BUCKETS, ITCM_LO, ITCM_BUCKETS, 0, 0, 0};
u16 port_pc_main[MAIN_BUCKETS];
u16 port_pc_itcm[ITCM_BUCKETS];
extern u32 __sp_irq[];

static void sample(void) {
    u32 pc = __sp_irq[-1] - 4;
    port_pc_header[5]++;
    if (pc - MAIN_LO < (MAIN_BUCKETS << 6)) { u16 *c = &port_pc_main[(pc - MAIN_LO) >> 6]; if (*c != 0xFFFF) ++*c; }
    else if (pc - ITCM_LO < (ITCM_BUCKETS << 6)) { u16 *c = &port_pc_itcm[(pc - ITCM_LO) >> 6]; if (*c != 0xFFFF) ++*c; }
    else port_pc_header[6]++;
}

void port_pc_sampler_init(void) {
    timerStart(1, ClockDivider_64, TIMER_FREQ_64(8000), sample);
}
#endif
