#define main background_unused_main
#include "background_probe.c"
#undef main
#include "effect_vm.h"
void gte_ctc2(int r,u32 v) { g[32+r]=v; }
u32 gte_cfc2(int r) { return g[32+r]; }
static u32 exported(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 h,u32 i,
                    u32 j,u32 k,u32 l,u32 m,u32 n,u32 o,u32 p,u32 q) {
    u32 v[]={a,b,c,d,e,f,h,i,j,k,l,m,n,o,p,q},sum=0;
    for(unsigned x=0;x<16;x++) sum+=v[x]*(x+1);
    return sum;
}
static u32 pointer_export(u32 input,u32 scalar,u32 output) {
    if(input!=(u32)(psx_ram+0x180000) || scalar!=0x80008000 || output!=(u32)(psx_ram+0x180004)) abort();
    *(u32 *)output=0xfeed1234;return 0x76543210;
}
static unsigned position_mask;
static u32 position_export(u32 id,u32 a,u32 b) {
    u32 expected_id=position_mask&1 ? (u32)(psx_ram+0x8000) : 0x80008000;
    u32 expected_a=position_mask&2 ? (u32)(psx_ram+0x180000) : 0x80180000;
    u32 expected_b=position_mask&4 ? (u32)(psx_ram+0x180004) : 0x80180004;
    if(id!=expected_id || a!=expected_a || b!=expected_b) abort();
    if(position_mask&2) *(u32 *)a=0x1234abcd;
    if(position_mask&4) *(u32 *)b=0x9876fedc;
    return 0x76543210;
}
static void position_bridge_test(void) {
    u32 *code=(u32 *)(psx_ram+0x1e7700),target=(u32)position_export;
    u32 *context=(u32 *)(psx_ram+0x170000),*table=(u32 *)(psx_ram+0x170100);
    static const unsigned entries[][2]={{0x1f8,2},{0x1fc,2},{0x200,4},
        {0x204,2},{0x20c,2},{0x214,6},{0x2a0,2},{0x2a4,2},{0x2a8,2},{0x2b8,4},
        {0x60,7},{0x64,7},{0x28c,7},{0x31c,7},{0x254,4},{0x290,4},
        {0xcc,3},{0x17c,3},{0x184,1},{0x188,1}};
    u32 words[]={0x27bdffc0,0xafbf003c,0x3c048000,0x34848000,
                 0x3c058018,0x34a50000,0x3c068018,0x34c60004,
                 0x3c190000|(target>>16),0x37390000|(target&65535),
                 0x0320f809,0,0x8fbf003c,0,0x03e00008,0x27bd0040};
    __builtin_memcpy(code,words,sizeof words);
    *(u32 *)(psx_ram+0xf78a4)=(u32)context;context[4]=(u32)table;
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++) {
        position_mask=entries[i][1];table[entries[i][0]/4]=target;
        *(u32 *)(psx_ram+0x180000)=0;*(u32 *)(psx_ram+0x180004)=0;
        if(port_effect_call(0x801e7700,0,0,0,0,0)!=0x76543210) abort();
        if((position_mask&2) && *(u32 *)(psx_ram+0x180000)!=0x1234abcd) abort();
        if((position_mask&4) && *(u32 *)(psx_ram+0x180004)!=0x9876fedc) abort();
        table[entries[i][0]/4]=0;
    }
    *(u32 *)(psx_ram+0xf78a4)=0;
}
static void pointer_bridge_test(void) {
    u32 *code=(u32 *)(psx_ram+0x1e7700),target=(u32)pointer_export;
    u32 *context=(u32 *)(psx_ram+0x170000),*table=(u32 *)(psx_ram+0x170100);
    *(u32 *)(psx_ram+0xf78a4)=(u32)context;context[4]=(u32)table;
    table[0x2b0/4]=target; /* beyond the formerly truncated export scan */
    u32 words[]={0x27bdffc0,0xafbf003c,0x3c048018,0x34840000,
                 0x3c058000,0x34a58000,0x3c068018,0x34c60004,
                 0x3c190000|(target>>16),0x37390000|(target&65535),
                 0x0320f809,0,0x8fbf003c,0,0x03e00008,0x27bd0040};
    __builtin_memcpy(code,words,sizeof words);
    if(port_effect_call(0x801e7700,0,0,0,0,0)!=0x76543210 || *(u32 *)(psx_ram+0x180004)!=0xfeed1234) abort();
    *(u32 *)(psx_ram+0xf78a4)=0;
    if(port_effect_data(0)!=0 || port_effect_data(0x80180000)!=(u32)(psx_ram+0x180000) ||
       port_effect_data((u32)(psx_ram+0x180000))!=(u32)(psx_ram+0x180000)) abort();
}
static u32 offscreen_export(u32 x,u32 y,u32 width,u32 height,u32 ot,u32 scalar) {
    if(x!=640 || y!=256 || width!=112 || height!=112 ||
       ot!=(u32)(psx_ram+0x180000) || scalar!=0x80181234) abort();
    *(u32 *)ot=0xabcdef01;return 0x76543210;
}
static void offscreen_bridge_test(void) {
    u32 *code=(u32 *)(psx_ram+0x1e7700),target=(u32)offscreen_export;
    u32 *context=(u32 *)(psx_ram+0x170000),*table=(u32 *)(psx_ram+0x170100);
    *(u32 *)(psx_ram+0xf78a4)=(u32)context;context[4]=(u32)table;table[0x2f4/4]=target;
    u32 words[]={0x27bdffc0,0xafbf003c,0x24040280,0x24050100,
                 0x24060070,0x24070070,0x3c088018,0xafa80010,
                 0x35081234,0xafa80014,0x3c190000|(target>>16),0x37390000|(target&65535),
                 0x0320f809,0,0x8fbf003c,0,0x03e00008,0x27bd0040};
    __builtin_memcpy(code,words,sizeof words);
    if(port_effect_call(0x801e7700,0,0,0,0,0)!=0x76543210 || *(u32 *)(psx_ram+0x180000)!=0xabcdef01) abort();
    table[0x2f4/4]=0;*(u32 *)(psx_ram+0xf78a4)=0;
}
static void bridge_test(void) {
    u32 *code=(u32 *)(psx_ram+0x1e7700),target=(u32)exported;
    /* Save RA, prepare 12 stack arguments, and call a native 16-word export. */
    unsigned n=0;code[n++]=0x27bdffb0;code[n++]=0xafbf004c;
    for(unsigned i=4;i<16;i++){code[n++]=0x24080000|(i+1);code[n++]=0xafa80000|(i*4);}
    code[n++]=0x3c190000|(target>>16);code[n++]=0x37390000|(target&65535);
    code[n++]=0x0320f809;code[n++]=0;
    code[n++]=0x8fbf004c;code[n++]=0;code[n++]=0x03e00008;code[n++]=0x27bd0050;
    u32 got=port_effect_call(0x801e7700,1,2,3,4,0),want=0;
    for(unsigned i=1;i<=16;i++) want+=i*i;
    if(got!=want) abort();
}
static unsigned declared_mask;
static u32 declared_export(u32 a0,u32 a1,u32 a2,u32 a3,u32 a4,u32 a5,u32 a6,u32 a7,
                           u32 a8,u32 a9,u32 a10,u32 a11,u32 a12,u32 a13,u32 a14,u32 a15) {
    u32 a[]={a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15};
    for(unsigned i=0;i<16;i++) {
        u32 expected=i==13 ? 0x801e8178 : 0x80180000+i*32;
        if(declared_mask&(1u<<i)) expected=(u32)PSX_ADDR(expected);
        if(a[i]!=expected) abort();
    }
    return 0x76543210;
}
static void declared_bridge_test(void) {
    /* Reviewed signatures cover fixed pointers, high stack arguments, and
     * E6764's raw function callback at argument 13. Other scalar words look
     * like guest pointers deliberately: the bridge must leave them intact. */
    static const unsigned entries[][2]={{0x0,3},{0x48,19},{0x50,1},{0xf8,39},
        {0x108,0xc02},{0x208,14},{0x248,0xe00},{0x340,11}};
    u32 *code=(u32 *)(psx_ram+0x1e7700),target=(u32)declared_export;
    u32 *context=(u32 *)(psx_ram+0x170000),*table=(u32 *)(psx_ram+0x170100);
    *(u32 *)(psx_ram+0xf78a4)=(u32)context;context[4]=(u32)table;
    unsigned n=0;code[n++]=0x27bdffb0;code[n++]=0xafbf004c;
    for(unsigned i=0;i<16;i++) {
        u32 value=i==13 ? 0x801e8178 : 0x80180000+i*32;
        unsigned reg=i<4 ? 4+i : 8;
        code[n++]=0x3c000000|(reg<<16)|(value>>16);
        code[n++]=0x34000000|(reg<<21)|(reg<<16)|(value&65535);
        if(i>=4) code[n++]=0xafa80000|i*4;
    }
    code[n++]=0x3c190000|(target>>16);code[n++]=0x37390000|(target&65535);
    code[n++]=0x0320f809;code[n++]=0;
    code[n++]=0x8fbf004c;code[n++]=0;code[n++]=0x03e00008;code[n++]=0x27bd0050;
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++) {
        declared_mask=entries[i][1];table[entries[i][0]/4]=target;
        if(port_effect_call(0x801e7700,0,0,0,0,0)!=0x76543210) abort();
        table[entries[i][0]/4]=0;
    }
    *(u32 *)(psx_ram+0xf78a4)=0;
}
static void incoming_words_test(void) {
    u32 *code=(u32 *)(psx_ram+0x1e7700);
    u32 args[16];
    for(unsigned i=0;i<16;i++) args[i]=0x87654321u+i*0x1234567u;
    for(unsigned count=6;count<=9;count++) {
        unsigned n=0;u32 sum=0,weighted=0;
        code[n++]=0x00851021; /* addu v0,a0,a1 */
        code[n++]=0x00461021;code[n++]=0x00471021;
        for(unsigned i=4;i<count;i++) {
            code[n++]=0x8fa80000|i*4; /* lw t0,i*4(sp) */
            code[n++]=0; /* load delay */
            code[n++]=0x00481021;
        }
        code[n++]=0x03e00008;code[n++]=0;
        for(unsigned i=0;i<count;i++) {sum+=args[i];weighted+=args[i]*(i+1);}
        if(port_effect_call_words(0x801e7700,args,count)!=sum) abort();
        if(port_effect_call_words((u32)exported,args,count)!=weighted) abort();
    }
}
int main(void) {
    if(mmap(psx_ram,PSX_RAM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=psx_ram) abort();
    bridge_test();
    pointer_bridge_test();
    incoming_words_test();
    position_bridge_test();
    offscreen_bridge_test();
    declared_bridge_test();
    u32 args[5],size;
    while(fread(&size,4,1,stdin)==1) {
        if(size>0x5000 || fread(args,20,1,stdin)!=1 || fread(psx_ram+0x1e7700,size,1,stdin)!=1 || fread(psx_ram+0x180000,128,1,stdin)!=1 || fread(g,sizeof g,1,stdin)!=1) abort();
        u32 result=port_effect_call(0x801e7700,args[0],args[1],args[2],args[3],args[4]);
        fwrite(&result,4,1,stdout);fwrite(psx_ram+0x180000,128,1,stdout);fwrite(g,sizeof g,1,stdout);
    }
}
