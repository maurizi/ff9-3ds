/* 800DF15C: interpolate morph-model vertices, compose each bone with the
 * camera matrix, then project in-place through the original GTE sequence. */
#include "psyq_port.h"
#include "gte.h"
extern u32 g_vtxScreenXY[], g_vtxScreenZ[];
static u32 word(const void *p,unsigned offset) {return *(const u32 *)((const u8 *)p+offset);}
static const u8 *pointer(const void *p,unsigned offset) {return (const u8 *)word(p,offset);}
static void load_matrix(const u8 *matrix) {
    for(unsigned i=0;i<8;i++) gte_ctc2(i,word(matrix,i*4));
}
static void load_vertices(unsigned first) {
    for(unsigned i=0;i<3;i++) {
        gte_mtc2(i*2,g_vtxScreenXY[first+i]);
        gte_mtc2(i*2+1,g_vtxScreenZ[first+i]);
    }
}
void o0b_func_800DF15C(const void *model,u32 mesh_index,u32 factor) {
    const u8 *header=pointer(model,8);
    unsigned offset=(mesh_index&255)*40;
    const u8 *source=pointer(header,16)+offset;
    const u8 *target=pointer(pointer(model,92),16)+offset;
    const u8 *gs=*(const u8 **)(void *)PSX_ADDR(0x8006794c);
    const u8 *data=pointer(gs,28),*bone=pointer(model,32);
    gte_ctc2(26,*(const u16 *)(data+10));
    const u16 *counts=(const u16 *)pointer(source,20);
    const s16 *from=(const s16 *)pointer(source,24);
    const s16 *to=(const s16 *)pointer(target,24);
    unsigned first=0;
    for(unsigned part=0;part<header[2];part++,bone+=32) {
        unsigned count=counts[part];
        if(!count) continue;
        u32 composed[8]={0};
        load_matrix(data+24);
        for(unsigned column=0;column<3;column++) {
            for(unsigned row=0;row<3;row++)
                gte_mtc2(9+row,*(const u16 *)(bone+(row*3+column)*2));
            gte_cmd(0x49e012); /* camera rotation * bone column, no translation */
            for(unsigned row=0;row<3;row++) {
                s16 value=(s16)gte_mfc2(9+row);
                __builtin_memcpy((u8 *)composed+(row*3+column)*2,&value,2);
            }
        }
        gte_mtc2(0,*(const u16 *)(bone+20)|((u32)*(const u16 *)(bone+24)<<16));
        gte_mtc2(1,word(bone,28));gte_cmd(0x480012);
        for(unsigned row=0;row<3;row++) composed[5+row]=gte_mfc2(25+row);
        gte_mtc2(8,factor);
        for(unsigned i=0;i<count;i++) {
            for(unsigned axis=0;axis<3;axis++) {
                gte_ctc2(21+axis,(u32)(s32)from[(first+i)*4+axis]);
                gte_mtc2(9+axis,(u32)(s32)to[(first+i)*4+axis]);
            }
            gte_cmd(0x980011);
            g_vtxScreenXY[first+i]=(gte_mfc2(25)&65535)|(gte_mfc2(26)<<16);
            g_vtxScreenZ[first+i]=gte_mfc2(27);
        }
        load_matrix((const u8 *)composed);
        load_vertices(first);
        /* Preserve the final speculative vector loads and writes for a
         * partial triple; they are observable in GTE state and vertex tails. */
        for(unsigned i=0;i<count;i+=3) {
            gte_cmd(0x280030);
            load_vertices(first+i+3);
            for(unsigned j=0;j<3;j++) {
                g_vtxScreenXY[first+i+j]=gte_mfc2(12+j);
                g_vtxScreenZ[first+i+j]=gte_mfc2(17+j);
            }
        }
        first+=count;
    }
}
