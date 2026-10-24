/* Calls between ovl_0b and the battle effect modules (config/patches.txt routes them here). Every module is native
 * (port/game/effect_native.c, all 380 from the decomp's C): a call into the module slot by PS1 address, the first
 * one into a newly loaded module (its header's raw op pointers), is answered with the native function at that
 * address, and the module's pointers are relocated then; other targets are native already. Data addresses the
 * modules hand over may still be PS1 addresses, converted where they are dereferenced. */
#include "psyq_port.h"
#include "effect_bridge.h"
#include "profile.h"
#include <stdint.h>
typedef u32 (*NativeCall)(u32,u32,u32,u32,u32,u32,u32,u32,u32,u32,u32,u32,u32,u32,u32,u32);
_Static_assert(sizeof(void *)==4, "effect bridge requires 32-bit native ABI");
static int guest_code(u32 a) { return a>=0x801e7700 && a<0x801f1700; }
static u8 *memory(u32 a) {
    if(a<PSX_RAM_SIZE || (a>=0x80000000 && a<0x80200000)) return (u8 *)PSX_ADDR(a);
    if(a>=0x1f800000 && a<0x1f800400) return psx_scratchpad+(a&0x3ff);
    return (u8 *)(uintptr_t)a;
}
/* Decode a pointer from module-owned data at its native dereference boundary.
 * Keep the stored value intact so module pointer comparisons remain valid. */
u32 port_effect_data(u32 address) {
    return address ? (u32)(uintptr_t)memory(address) : 0;
}
/* effect_native.c (weak: the battle probes link without it) */
__attribute__((weak)) u32 port_fxmod_native(u32 target) { (void)target; return 0; }
u32 port_effect_call_words(u32 target,const u32 *arguments,u32 count) {
    unsigned int start=port_profile_begin();
    if(count>16) port_fatal("effect call: too many arguments");
    if(guest_code(target)) {
        u32 native=port_fxmod_native(target);
        if(!native) port_fatal("effect call: no native function at %08X (module pack missing or stale?)",target);
        target=native;
    }
    if(!target) port_fatal("effect call: null target");
    u32 a[16]={0};for(unsigned i=0;i<count;i++) a[i]=arguments[i];
    u32 result=((NativeCall)(uintptr_t)target)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
                                               a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15]);
    port_profile_end(PORT_PROFILE_VM,start);
    return result;
}
u32 port_effect_call(u32 target,u32 a0,u32 a1,u32 a2,u32 a3,u32 a4) {
    u32 arguments[5]={a0,a1,a2,a3,a4};
    return port_effect_call_words(target,arguments,5);
}
