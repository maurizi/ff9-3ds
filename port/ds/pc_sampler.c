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

/* memset's callers of 16 bytes or more (linked with --wrap=memset): caller, calls, bytes */
#define MEMSET_CALLERS 256
volatile u32 port_pc_memset[MEMSET_CALLERS][3];
void *__real_memset(void *p, int c, size_t n);
void *__wrap_memset(void *p, int c, size_t n) {
    if (n >= 16) {
        u32 caller = (u32)__builtin_return_address(0);
        for (u32 i = (caller >> 1) * 2654435761u >> 24, tries = 0; tries < 8; ++tries, i = (i + 1) & 255)
            if (port_pc_memset[i][0] == caller || !port_pc_memset[i][0]) {
                port_pc_memset[i][0] = caller; port_pc_memset[i][1]++; port_pc_memset[i][2] += n;
                break;
            }
    }
    return __real_memset(p, c, n);
}

void port_pc_sampler_init(void) {
    timerStart(1, ClockDivider_64, TIMER_FREQ_64(8000), sample);
}
#endif
