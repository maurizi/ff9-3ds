/* CD-loaded effect modules contain MIPS independently of resident overlays.
 * Execute their glue against the existing native integer/pointer-word engine
 * exports. This is not a replacement for the native game/graphics engines. */
#include "psyq_port.h"
#include "gte.h"
#include "effect_vm.h"
#include "profile.h"
#include <stdint.h>
typedef u32 (*NativeExport)(u32,u32,u32,u32,u32,u32,u32,u32,
                           u32,u32,u32,u32,u32,u32,u32,u32);
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
static u32 read32(u32 a) { u32 v;__builtin_memcpy(&v,memory(a),4);return v; }
static u32 read16(u32 a) { u16 v;__builtin_memcpy(&v,memory(a),2);return v; }
static void write32(u32 a,u32 v) { __builtin_memcpy(memory(a),&v,4); }
static void write16(u32 a,u32 v) { u16 h=(u16)v;__builtin_memcpy(memory(a),&h,2); }
static u32 native_call(u32 target,u32 *r) {
    if(!target) port_fatal("effect VM: null engine export");
    u32 a[16];for(unsigned i=0;i<4;i++) a[i]=r[4+i];
    for(unsigned i=4;i<16;i++) a[i]=read32(r[29]+i*4);
    /* Reviewed export argument types. D044C's second argument is a pointer
     * carried as s32; scalar flags must never be guessed to be pointers. */
    /* 216 function exports precede the pool/data pointers at table +0x360. */
    static const u16 pointer_mask[216]={
        [0x8/4]=0xd, /* E0014: particle, position bias, output vector. */
        [0x14/4]=3,[0x20/4]=3,[0xbc/4]=11,[0xd4/4]=7,
        [0xf0/4]=11,[0x110/4]=0x85,[0x114/4]=7,[0x134/4]=1,
        [0x234/4]=4,[0x27c/4]=1,
        /* Model-pool initialization receives descriptors within the module. */
        [0x70/4]=1,[0x74/4]=1,
        [0xd8/4]=5,[0xdc/4]=11,[0xe0/4]=4,
        [0x2b0/4]=5,[0x2b4/4]=3,[0x338/4]=1,
        [0x13c/4]=6,[0x148/4]=2,[0x14c/4]=15,
        /* These s32 wrappers forward descriptor/position pointers to the pools. */
        [0x1d4/4]=3,[0x1d8/4]=3,
        /* Unit IDs stay scalar; position/rotation/vector buffers are pointers. */
        [0x1f8/4]=2,[0x1fc/4]=2,[0x200/4]=4,[0x204/4]=2,
        [0x20c/4]=2,[0x214/4]=6,[0x2a0/4]=2,[0x2a4/4]=2,[0x2a8/4]=2,[0x2b8/4]=4,
        /* Draw-model wrappers carry scale pointers as s32 before DCA68. */
        [0x60/4]=7,[0x64/4]=7,[0x28c/4]=7,[0x31c/4]=7,
        [0x254/4]=4,[0x290/4]=4,
        /* Vector metric and custom work-pool APIs; callback words stay code. */
        [0xcc/4]=3,[0x17c/4]=3,[0x184/4]=1,[0x188/4]=1,
        [0x2f4/4]=16, /* E6E1C: stack argument 4 is the offscreen OT. */
        /* SplitMdlVertex's scalar-typed third word is a writable vertex
         * buffer, retained by DF940 and dereferenced by SPLIT_PRIMS. */
        [0x304/4]=4,
        /* Declared data pointers from the generated export table and native
         * DWARF signatures. Function callbacks remain guest code addresses.
         * Recheck with tools/audit_effect_exports.py after signature changes. */
        [0x50/4]=1, /* printf: fixed format parameter; variadic tail stays raw. */
        [0x0/4]=0x3, /* LoadImage */
        [0x4/4]=0x3, /* StoreImage */
        [0xc/4]=0x600, /* o0b_func_800D12C4 */
        [0x18/4]=0x1, /* o0b_Hi_RegisterGouEffModel */
        [0x1c/4]=0x3, /* SetDrawMove */
        [0x24/4]=0x7, /* CompMatrix */
        [0x28/4]=0x1, /* SetRotMatrix */
        [0x34/4]=0x1, /* SetTransMatrix */
        [0x48/4]=0x13, /* LoadAverageShort12 */
        [0x4c/4]=0x7, /* o0b_Hi_RegisterTexListModel */
        [0x54/4]=0x1, /* o0b_Hi_RegisterSolidEffModel */
        [0x58/4]=0x1, /* o0b_Hi_RegisterTexEffModel */
        [0x5c/4]=0x3, /* o0b_Hi_RegisterSummonModel */
        [0x68/4]=0x1, /* o0b_Hi_SetSummonMotion */
        [0x6c/4]=0x1, /* o0b_func_800E8854 */
        [0x80/4]=0x4, /* o0b_func_800DC818 */
        [0x84/4]=0x4, /* o0b_func_800DC858 */
        [0xd0/4]=0x1, /* o0b_btlfx_randRotation */
        [0xe4/4]=0x3, /* o0b_btlfx_turnToward */
        [0xe8/4]=0x3, /* o0b_func_800D0130 */
        [0xec/4]=0x7, /* o0b_func_800D01D4 */
        [0xf4/4]=0x3, /* o0b_func_800D0580 */
        [0xf8/4]=0x27, /* o0b_func_800D0674 */
        [0x100/4]=0x1, /* o0b_func_800D0A0C */
        [0x108/4]=0xc02, /* o0b_func_800D0E14 */
        [0x10c/4]=0x600, /* o0b_func_800D0FFC */
        [0x118/4]=0x3, /* o0b_func_800D198C */
        [0x11c/4]=0x2, /* o0b_func_800D1BC4 */
        [0x120/4]=0x7, /* o0b_func_800D1D74 */
        [0x124/4]=0x7, /* o0b_func_800D1FFC */
        [0x128/4]=0x7, /* o0b_func_800D23D8 */
        [0x12c/4]=0x7, /* o0b_func_800D2938 */
        [0x130/4]=0x1, /* o0b_func_800D2DBC */
        [0x138/4]=0x3, /* o0b_func_800D3B6C */
        [0x140/4]=0x3, /* o0b_func_800E0134 */
        [0x144/4]=0x1, /* o0b_func_800E0168 */
        [0x150/4]=0x1, /* o0b_func_800E1708 */
        [0x154/4]=0x3, /* o0b_func_800E1810 */
        [0x158/4]=0x3, /* o0b_func_800E1874 */
        [0x15c/4]=0x1, /* o0b_func_800E1F90 */
        [0x160/4]=0x3, /* o0b_func_800E2000 */
        [0x164/4]=0xf, /* o0b_func_800E2F84 */
        [0x16c/4]=0x1, /* o0b_func_800D6D8C */
        [0x180/4]=0x1, /* o0b_func_800DA1B8 */
        [0x1bc/4]=0x1, /* o0b_func_800E3B34 */
        [0x1c0/4]=0x1, /* o0b_func_800E3B70 */
        [0x1c4/4]=0x1, /* o0b_func_800E3BF8 */
        [0x1c8/4]=0x1, /* o0b_func_800E3C50 */
        [0x1dc/4]=0x1, /* o0b_func_800E3D54 */
        [0x1e0/4]=0x1, /* o0b_func_800E701C */
        [0x208/4]=0xe, /* o0b_func_800E82CC */
        [0x218/4]=0x4, /* o0b_func_800E7BA0 */
        [0x22c/4]=0x4, /* o0b_func_800D08C4 */
        [0x238/4]=0x3, /* o0b_func_800E651C */
        [0x23c/4]=0x6, /* o0b_func_800D07B4 */
        [0x248/4]=0xe00, /* o0b_func_800E6764 */
        [0x250/4]=0x1, /* o0b_func_800E6C58 */
        [0x264/4]=0x40, /* o0b_func_800D0B18 */
        [0x294/4]=0x1, /* ClearImage */
        [0x298/4]=0x1, /* MoveImage */
        [0x2ac/4]=0x7, /* o0b_Hi_RegisterTexPtrModel */
        [0x2c4/4]=0x4, /* o0b_func_800DC8AC */
        [0x2c8/4]=0x4, /* o0b_func_800DC8EC */
        [0x2e0/4]=0x2, /* o0b_func_800E871C */
        [0x310/4]=0x8, /* o0b_Hi_GetMdlVertexPtr */
        [0x330/4]=0x20, /* o0b_func_800CFC2C */
        [0x340/4]=0xb, /* o0b_func_800E9000 */
        [0x35c/4]=0x3, /* o0b_func_800DD120 */
    };
    u32 ctx=read32((u32)PSX_ADDR(0x800f78a4));
    if(ctx) {
        u32 table=read32(ctx+16);
        for(unsigned index=0;index<216;index++) if(read32(table+index*4)==target) {
            u16 mask=pointer_mask[index];
            for(unsigned i=0;i<16;i++) if(a[i] && (mask&(1u<<i))) a[i]=(u32)(uintptr_t)memory(a[i]);
            break;
        }
    }
    return ((NativeExport)(uintptr_t)target)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
                                            a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15]);
}
#ifdef PORT_PROFILE
#define port_effect_call_words effect_call_words_impl
#endif
u32 port_effect_call_words(u32 target,const u32 *arguments,u32 count) {
    if(count>16) port_fatal("effect VM: too many callback arguments");
    if(!guest_code(target)) {
        if(!target) port_fatal("effect VM: null effect callback");
        u32 a[16]={0};for(unsigned i=0;i<count;i++) a[i]=arguments[i];
        return ((NativeExport)(uintptr_t)target)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
                                                a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15]);
    }
    /* Real native stack pointers allow C exports to consume local structures.
     * Space above initial SP holds o32 arguments; space below holds frames. */
    u32 stack[1024]={0},r[32]={0},hi=0,lo=0;
    for(unsigned i=0;i<count && i<4;i++) r[4+i]=arguments[i];
    r[29]=(u32)(uintptr_t)(stack+992);r[31]=0xffffffff;
    for(unsigned i=4;i<count;i++) write32(r[29]+i*4,arguments[i]);
    u32 pc=target,delayed=0;int has_delay=0;
    for(unsigned steps=0;steps<1000000;steps++) {
        if(pc==0xffffffff) return r[2];
        if(!guest_code(pc)) {
            if(pc>=0x80000000 && pc<0x80200000) port_fatal("effect VM: unresolved guest call %08X",pc);
            r[2]=native_call(pc,r);pc=r[31];continue;
        }
        if(pc&3) port_fatal("effect VM: unaligned PC %08X",pc);
        u32 ins=read32(pc),op=ins>>26;
        unsigned rs=(ins>>21)&31,rt=(ins>>16)&31,rd=(ins>>11)&31;
        unsigned sh=(ins>>6)&31,fun=ins&63;
        u32 imm=ins&65535,a=r[rs],b=r[rt],addr=a+(u32)(s32)(s16)imm;
        u32 next=has_delay?delayed:pc+4,branch=0;int take=0;has_delay=0;
        switch(op) {
        case 0:
            switch(fun) {
            case 0:r[rd]=b<<sh;break;case 2:r[rd]=b>>sh;break;case 3:r[rd]=(u32)((s32)b>>sh);break;
            case 4:r[rd]=b<<(a&31);break;case 6:r[rd]=b>>(a&31);break;case 7:r[rd]=(u32)((s32)b>>(a&31));break;
            case 8:branch=a;take=1;break;case 9:r[rd]=pc+8;branch=a;take=1;break;
            case 16:r[rd]=hi;break;case 17:hi=a;break;case 18:r[rd]=lo;break;case 19:lo=a;break;
            case 24:{int64_t v=(int64_t)(s32)a*(s32)b;lo=(u32)v;hi=(u32)((uint64_t)v>>32);break;}
            case 25:{uint64_t v=(uint64_t)a*b;lo=(u32)v;hi=(u32)(v>>32);break;}
            case 26:
                if(!b){lo=(s32)a<0?1:0xffffffff;hi=a;}
                else if(a==0x80000000 && b==0xffffffff){lo=a;hi=0;}
                else {lo=(u32)((s32)a/(s32)b);hi=(u32)((s32)a%(s32)b);}break;
            case 27:if(!b){lo=0xffffffff;hi=a;}else{lo=a/b;hi=a%b;}break;
            case 32:case 33:r[rd]=a+b;break;case 34:case 35:r[rd]=a-b;break;
            case 36:r[rd]=a&b;break;case 37:r[rd]=a|b;break;case 38:r[rd]=a^b;break;case 39:r[rd]=~(a|b);break;
            case 42:r[rd]=(s32)a<(s32)b;break;case 43:r[rd]=a<b;break;
            default:goto unsupported;
            }break;
        case 1:
            if(rt==0 || rt==16) take=(s32)a<0;else if(rt==1 || rt==17) take=(s32)a>=0;else goto unsupported;
            if(rt>=16) r[31]=pc+8;
            branch=pc+4+((u32)(s32)(s16)imm<<2);break;
        case 2:case 3:if(op==3)r[31]=pc+8;branch=((pc+4)&0xf0000000)|((ins&0x3ffffff)<<2);take=1;break;
        case 4:take=a==b;goto conditional;case 5:take=a!=b;goto conditional;
        case 6:take=(s32)a<=0;goto conditional;case 7:take=(s32)a>0;
        conditional:branch=pc+4+((u32)(s32)(s16)imm<<2);break;
        case 8:case 9:r[rt]=addr;break;
        case 10:r[rt]=(s32)a<(s32)(s16)imm;break;case 11:r[rt]=a<(u32)(s32)(s16)imm;break;
        case 12:r[rt]=a&imm;break;case 13:r[rt]=a|imm;break;case 14:r[rt]=a^imm;break;case 15:r[rt]=imm<<16;break;
        case 18:
            if(ins&(1u<<25)) gte_cmd(ins&0x1ffffff);
            else if(rs==0) r[rt]=gte_mfc2(rd);else if(rs==2) r[rt]=gte_cfc2(rd);
            else if(rs==4) gte_mtc2(rd,b);else if(rs==6) gte_ctc2(rd,b);else goto unsupported;
            break;
        case 32:r[rt]=(u32)(s32)(s8)*memory(addr);break;case 33:r[rt]=(u32)(s32)(s16)read16(addr);break;
        case 35:r[rt]=read32(addr);break;case 36:r[rt]=*memory(addr);break;case 37:r[rt]=read16(addr);break;
        case 40:*memory(addr)=(u8)b;break;case 41:write16(addr,b);break;case 43:write32(addr,b);break;
        case 50:gte_mtc2(rt,read32(addr));break;case 58:write32(addr,gte_mfc2(rt));break;
        default:goto unsupported;
        }
        if(r[29]<(u32)(uintptr_t)stack || r[29]>(u32)(uintptr_t)(stack+992))
            port_fatal("effect VM: stack outside reserved frames at %08X",pc);
        r[0]=0;if(take){delayed=branch;has_delay=1;}pc=next;continue;
    unsupported:port_fatal("effect VM: unsupported instruction %08X at %08X",ins,pc);
    }
    port_fatal("effect VM: instruction limit at %08X",pc);return 0;
}
#ifdef PORT_PROFILE
#undef port_effect_call_words
u32 port_effect_call_words(u32 target,const u32 *arguments,u32 count) {
    unsigned int start=port_profile_begin();
    u32 result=effect_call_words_impl(target,arguments,count);
    port_profile_end(PORT_PROFILE_VM,start);
    return result;
}
#endif
u32 port_effect_call(u32 target,u32 a0,u32 a1,u32 a2,u32 a3,u32 a4) {
    u32 arguments[5]={a0,a1,a2,a3,a4};
    return port_effect_call_words(target,arguments,5);
}
