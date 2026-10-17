/* Debug-only MPU guard: catch the first write into raster instructions.
 * Region 7 normally covers DTCM, which also has RW coverage from region 6.
 * Reusing it preserves DTCM access while overriding region 4 for an immutable
 * prefix of ITCM. Its size follows the link layout so IRQ data stays writable. */
#include <nds.h>
#include <stdio.h>

/* The stock handler decodes the faulting instruction to infer its address.
 * Preserve the first exception before any decoder/console fault can replace
 * libnds' shared register array. This is only installed in guard builds. */
volatile unsigned port_itcm_first_fault[21];
volatile unsigned port_itcm_guard_bytes;
extern unsigned short irq_nesting_level;

static void guard_exception(void) {
    if (!port_itcm_first_fault[0]) {
        for (unsigned i=0; i<16; ++i)
            port_itcm_first_fault[i+1]=(unsigned)exceptionRegisters[i];
        volatile unsigned *bios=(volatile unsigned *)0x02fffd90;
        for (unsigned i=0; i<3; ++i)
            port_itcm_first_fault[i+17]=bios[i];
        __asm__ volatile("mrs %0, cpsr" : "=r"(port_itcm_first_fault[20]));
        port_itcm_first_fault[0]=1;
    }
    REG_IME=0;
    consoleClear();
    printf("First DS exception preserved\n");
    printf("pc %08lx cpsr %08lx\n",
           (unsigned long)port_itcm_first_fault[16],
           (unsigned long)port_itcm_first_fault[17]);
    for (unsigned i=0; i<16; ++i)
        printf("r%-2u %08lx\n", i, (unsigned long)port_itcm_first_fault[i+1]);
    for (;;) __asm__ volatile("nop");
}

void port_itcm_guard_init(void) {
    unsigned data, instruction;
    /* libnds places its first mutable IRQ word after the raster instructions.
     * GX links fewer raster functions, so this word can fall below 16 KiB.
     * The ARM946 MPU permits power-of-two regions: choose the largest prefix
     * up to 16 KiB that stops before that word. */
    unsigned immutable=(unsigned)&irq_nesting_level-0x01000000u;
    unsigned bytes=16384, region=0x0100001b;
    while(bytes>immutable && bytes>4096) { bytes>>=1; region-=2; }
    sassert(immutable>=4096,"ITCM alias guard overlaps mutable IRQ data");
#ifdef PORT_ITCM_GUARD_LOW
    /* ITCMGUARD=2: instead make the whole 32 KiB alias at address 0 read-only. Code and IRQ data are only
     * written through their linked 0x01000000 addresses; a near-NULL pointer (base 0 plus a struct offset up to
     * 32 KiB) writes through this alias and can reach the IRQ table at the end of ITCM. */
    bytes=32768;
    region=0x0000001d; /* base 0, size 2^15, enabled */
#endif
    port_itcm_guard_bytes=bytes;
    unsigned ime=enterCriticalSection();
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 2\n"
                     "mrc p15, 0, %1, c5, c0, 3"
                     : "=r"(data), "=r"(instruction));
    /* Region 2 already covers 0..4095. ITCM is mirrored there, so a
     * corrupt near-null primitive pointer can overwrite the same code
     * without using its linked 0x01000000 address. Keep reads permitted
     * but disallow writes through that alias as well. */
    data=(data & 0x0ffff0ff) | 0x60000600;
    instruction=(instruction & 0x0fffffff) | 0x60000000;
    __asm__ volatile("mcr p15, 0, %0, c5, c0, 2\n"
                     "mcr p15, 0, %1, c5, c0, 3\n"
                     "mcr p15, 0, %2, c6, c7, 0\n"
                     "mcr p15, 0, %2, c7, c5, 4"
                     : : "r"(data), "r"(instruction), "r"(region) : "memory");
    leaveCriticalSection(ime);
    setExceptionHandler(guard_exception);
}
