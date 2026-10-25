/* PROFILE=3 (DSi builds): a sampling profiler. Timer 1 interrupts 8000 times a second and counts the interrupted
 * instruction's address in 64-byte buckets; tools/ds_pc_profile.py reads the counts from a melonDS state and maps
 * them to functions. The BIOS's IRQ entry pushes {r0-r3, r12, lr} at the top of the IRQ stack: lr - 4 is the
 * interrupted instruction (approximately, in Thumb code), whatever the libnds dispatcher does next. */
#ifdef PORT_PC_SAMPLER
#include <nds.h>

#ifdef PORT_PC_LO  /* PCLO=ADDR PCSHIFT=N: a window of 2560 fine buckets from ADDR (one function's lines) */
#define MAIN_LO PORT_PC_LO
#define SHIFT PORT_PC_SHIFT
#define MAIN_BUCKETS 2560u
#else
#define MAIN_LO 0x02000000u
#ifdef PORT_DS_MODE
#define SHIFT 10  /* 1 KB buckets: 5 KB, what DS mode's heap can spare */
#else
#define SHIFT 6
#endif
#define MAIN_BUCKETS ((0x022A0000u - MAIN_LO) >> SHIFT)
#endif
#define ITCM_LO 0x01000000u   /* (BlocksDS links ITCM here, __itcm_start) */
#ifdef PORT_PC_LO
#define ITCM_SHIFT 5  /* 32 bytes in a window build: the interrupt dispatcher apart from its neighbours */
#else
#define ITCM_SHIFT SHIFT
#endif
#define ITCM_BUCKETS (0x8000u >> ITCM_SHIFT)
/* header: magic, main base, main buckets, ITCM base, ITCM buckets, samples, outside (BIOS waits, other memory),
 * main bucket shift, ITCM bucket shift */
volatile u32 port_pc_header[9] = {0x53504331, MAIN_LO, MAIN_BUCKETS, ITCM_LO, ITCM_BUCKETS, 0, 0, SHIFT, ITCM_SHIFT};
u16 port_pc_main[MAIN_BUCKETS];
u16 port_pc_itcm[ITCM_BUCKETS];
extern u32 __sp_irq[];

static void sample(void) {
    u32 pc = __sp_irq[-1] - 4;
    port_pc_header[5]++;
    if (pc - MAIN_LO < (MAIN_BUCKETS << SHIFT)) { u16 *c = &port_pc_main[(pc - MAIN_LO) >> SHIFT]; if (*c != 0xFFFF) ++*c; }
    else if (pc - ITCM_LO < (ITCM_BUCKETS << ITCM_SHIFT)) { u16 *c = &port_pc_itcm[(pc - ITCM_LO) >> ITCM_SHIFT]; if (*c != 0xFFFF) ++*c; }
    else port_pc_header[6]++;
}

/* memset's callers of 16 bytes or more (linked with --wrap=memset): caller, calls, bytes */
#ifdef PORT_DS_MODE
#define MEMSET_CALLERS 64
#else
#define MEMSET_CALLERS 256
#endif
volatile u32 port_pc_memset[MEMSET_CALLERS][3];
void *__real_memset(void *p, int c, size_t n);
void *__wrap_memset(void *p, int c, size_t n) {
    if (n >= 16) {
        u32 caller = (u32)__builtin_return_address(0);
        for (u32 i = (caller >> 1) * 2654435761u % MEMSET_CALLERS, tries = 0; tries < 8; ++tries, i = (i + 1) % MEMSET_CALLERS)
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
