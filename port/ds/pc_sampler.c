/* PROFILE=3 (DSi builds): a sampling profiler. Timer 1 interrupts 8000 times a second and counts the interrupted
 * instruction's address in 64-byte buckets; tools/ds_pc_profile.py reads the counts from a melonDS state and maps
 * them to functions. The BIOS's IRQ entry pushes {r0-r3, r12, lr} at the top of the IRQ stack: lr - 4 is the
 * interrupted instruction (approximately, in Thumb code), whatever the libnds dispatcher does next. */
#ifdef PORT_PC_SAMPLER
#include <nds.h>

#ifdef PORT_PC_LO  /* PCLO=ADDR PCSHIFT=N: a window of 2560 fine buckets from ADDR (one function's lines) */
#define MAIN_LO PORT_PC_LO
#define SHIFT PORT_PC_SHIFT
#ifdef PORT_DS_MODE
#define MAIN_BUCKETS 1280u /* (what DS mode's heap can spare: PCSHIFT=6 covers 80 KB) */
#else
#define MAIN_BUCKETS 2560u
#endif
#else
#define MAIN_LO 0x02000000u
#ifdef PORT_DS_MODE
#define SHIFT 11  /* 2 KB buckets: 2.7 KB, what DS mode's heap can spare */
#else
#define SHIFT 6
#endif
#define MAIN_BUCKETS ((0x022A0000u - MAIN_LO) >> SHIFT)
#endif
#define ITCM_LO 0x01000000u   /* (BlocksDS links ITCM here, __itcm_start) */
#if defined(PORT_PC_LO) && defined(PORT_DS_MODE)
#define ITCM_SHIFT 9
#elif defined(PORT_PC_LO)
#define ITCM_SHIFT 6  /* 64 bytes in a window build (2 KB of 32-bit counts): the interrupt dispatcher apart from its neighbours */
#else
#define ITCM_SHIFT SHIFT
#endif
#define ITCM_BUCKETS (0x8000u >> ITCM_SHIFT)
/* header: magic, main base, main buckets, ITCM base, ITCM buckets, samples, outside (BIOS waits, other memory),
 * main bucket shift, ITCM bucket shift */
volatile u32 port_pc_header[9] = {0x53504332, /* 2: ITCM counts are 32-bit */ MAIN_LO, MAIN_BUCKETS, ITCM_LO, ITCM_BUCKETS, 0, 0, SHIFT, ITCM_SHIFT};
/* (in DTCM: the stack has ~7.9 KB to spare there, the heap too little in battles, where effect packs load) */
u16 port_pc_main[MAIN_BUCKETS] DTCM_BSS;
u32 port_pc_itcm[ITCM_BUCKETS]; /* 32-bit: the VBlank wait and the interrupt dispatcher share a few buckets */
extern u32 __sp_irq[];

/* counting from this many frames (platform.c): CAPTURE_FRAME's second half, the scene without its loading */
volatile u32 port_pc_from_frame;
extern volatile u32 port_pc_frame;
static void sample(void) {
    if (port_pc_frame < port_pc_from_frame) return;
    u32 pc = __sp_irq[-1] - 4;
    port_pc_header[5]++;
    if (pc - MAIN_LO < (MAIN_BUCKETS << SHIFT)) { u16 *c = &port_pc_main[(pc - MAIN_LO) >> SHIFT]; if (*c != 0xFFFF) ++*c; }
    else if (pc - ITCM_LO < (ITCM_BUCKETS << ITCM_SHIFT)) ++port_pc_itcm[(pc - ITCM_LO) >> ITCM_SHIFT];
    else port_pc_header[6]++;
}

/* memset's callers of 16 bytes or more (linked with --wrap=memset): caller, calls, bytes */
#ifdef PORT_DS_MODE
#define MEMSET_CALLERS 64
#else
#define MEMSET_CALLERS 256
#endif
volatile u32 port_pc_memset[MEMSET_CALLERS][3] DTCM_BSS;
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

/* free's callers (linked with --wrap=free): caller, calls */
volatile u32 port_pc_free[16][2];
void __real_free(void *p);
void __wrap_free(void *p) {
    u32 caller = (u32)__builtin_return_address(0);
    for (u32 i = (caller >> 1) % 16, tries = 0; tries < 16; ++tries, i = (i + 1) % 16)
        if (port_pc_free[i][0] == caller || !port_pc_free[i][0]) { port_pc_free[i][0] = caller; port_pc_free[i][1]++; break; }
    __real_free(p);
}

/* Who waits (linked with --wrap=swiIntrWait,--wrap=swiWaitForVBlank): caller, calls, timer 2/3 ticks waited */
#define WAIT_CALLERS 16
volatile u32 port_pc_wait[WAIT_CALLERS][3];
static u32 wait_clock(void) {
    u32 high, low;
    do { high = TIMER_DATA(3); low = TIMER_DATA(2); } while (high != TIMER_DATA(3));
    return high << 16 | low;
}
static void wait_note(u32 caller, u32 ticks) {
    for (u32 i = (caller >> 1) % WAIT_CALLERS, tries = 0; tries < WAIT_CALLERS; ++tries, i = (i + 1) % WAIT_CALLERS)
        if (port_pc_wait[i][0] == caller || !port_pc_wait[i][0]) {
            port_pc_wait[i][0] = caller; port_pc_wait[i][1]++; port_pc_wait[i][2] += ticks;
            return;
        }
}
void __real_swiIntrWait(u32 wait, u32 flags);
void __wrap_swiIntrWait(u32 wait, u32 flags) {
    u32 caller = (u32)__builtin_return_address(0), t = wait_clock();
    __real_swiIntrWait(wait, flags);
    wait_note(caller, wait_clock() - t);
}
void __real_swiWaitForVBlank(void);
void __wrap_swiWaitForVBlank(void) {
    u32 caller = (u32)__builtin_return_address(0), t = wait_clock();
    __real_swiWaitForVBlank();
    wait_note(caller | 1u << 31, wait_clock() - t); /* bit 31: through swiWaitForVBlank */
}

void port_pc_sampler_init(void) {
#ifdef PORT_CAPTURE_FRAME
    port_pc_from_frame = PORT_CAPTURE_FRAME / 2;
#endif
    timerStart(1, ClockDivider_64, TIMER_FREQ_64(8000), sample);
}
#endif
