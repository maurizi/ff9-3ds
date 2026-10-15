/* Handwritten 800D5C30: project a circular chain, derive ribbon width using
 * OP/SQR and the game's reciprocal-root table, then emit joined GT4s. */
#include "psyq_port.h"
#include "gte.h"
#include "effect_vm.h"
#include <stdint.h>
static u32 load(const void *p,unsigned offset) { return *(const u32 *)((const u8 *)p+offset); }
static void vertex(unsigned reg,const u8 *p) { gte_mtc2(reg,load(p,0));gte_mtc2(reg+1,load(p,4)); }
static void xy_add(u8 *out,u32 xy,s32 dx,s32 dy) {
    *(u16 *)out=(u16)((s32)(s16)xy+dx);
    *(u16 *)(out+2)=(u16)((s32)(s16)(xy>>16)+dy);
}
/* DFF5C and DFF64 share the body; only the velocity accumulator differs. */
static void advance_particle(u8 *particle,s32 speed,const s16 *direction,s16 *out,unsigned velocity_offset) {
    s32 rate=(s8)particle[30];
    gte_mtc2(8,(u32)rate<<6);
    for(unsigned i=0;i<3;i++) gte_mtc2(9+i,(u32)(s32)direction[i]);
    gte_cmd(0x198003d);
    u32 product=(u32)((int64_t)speed*rate);
    u32 velocity=(u32)(s32)*(s16 *)(particle+velocity_offset)+(u32)((s32)product>>7);
    *(s16 *)(particle+velocity_offset)=(s16)velocity;
    for(unsigned i=0;i<3;i++) {
        u32 delta=gte_mfc2(25+i);if(i==1) delta+=velocity;
        u32 value=load(particle,i*4)+(delta<<6);
        *(u32 *)(particle+i*4)=value;out[i]=(s16)((s32)value>>12);
    }
}
void o0b_func_800DFF5C(u8 *particle,s32 speed,const s16 *direction,s16 *out) {
    advance_particle(particle,speed,direction,out,16);
}
void o0b_func_800DFF64(u8 *particle,s32 speed,const s16 *direction,s16 *out) {
    advance_particle(particle,speed,direction,out,36);
}
/* 3D ribbon: the width direction is the normalized cross product of each
 * segment and its view vector. Preserve OP/SQR/GPF register side effects and
 * the original RTPT's existing MAC0 cull; it never issues NCLIP. */
void o0b_func_800D50A0(s32 count,void *context,u8 *out,u32 *ot) {
    u32 *c=context;
    const u8 *ring=(const u8 *)port_effect_data(c[1]);
    u32 fade=c[2],color=c[3],previous_color=color;
    u32 uvbase=*(u16 *)((u8 *)c+20),uv=uvbase;
    unsigned size=ring[0]+1,index=ring[1];ring+=4;
    gte_ctc2(21,(c[4]&255)<<4);
    gte_ctc2(22,(c[4]>>4)&0xff0);
    gte_ctc2(23,(c[4]>>12)&0xff0);
    c[12]=gte_cfc2(0);c[13]=gte_cfc2(2);c[14]=gte_cfc2(4);
    for(s32 segment=0;!segment || segment<count;segment++,out+=52) {
        unsigned next=index+1==size ? 0 : index+1;
        const s16 *a=(const s16 *)(ring+index*8),*b=(const s16 *)(ring+next*8);
        for(unsigned i=0;i<3;i++) {
            c[17+i]=(u32)(s32)a[i];c[20+i]=(u32)(s32)b[i];
            gte_ctc2(i*2,((u32)(s32)b[i]-(u32)(s32)a[i])*4);
            gte_mtc2(9+i,(u32)(s32)*(const s16 *)((u8 *)c+40+i*2)-(u32)(s32)a[i]);
        }
        gte_cmd(0x178000c);
        u32 normal[3]={gte_mfc2(25),gte_mfc2(26),gte_mfc2(27)};
        gte_cmd(0xa00428);
        u32 sum=gte_mfc2(25)+gte_mfc2(26)+gte_mfc2(27);
        gte_mtc2(30,sum);
        u32 leading=gte_mfc2(31)&~1u;
        unsigned shift=((u32)(31-leading)>>1)&31;
        u32 normalized=leading>=24 ? sum<<((leading-24)&31)
            : (u32)((s32)sum>>((24-leading)&31));
        s16 reciprocal=*(s16 *)(u32)PSX_ADDR(0x800cfa80+(normalized-64)*2);
        for(unsigned i=0;i<3;i++) gte_mtc2(9+i,normal[i]);
        gte_mtc2(8,(u32)(s32)reciprocal);gte_cmd(0x190003d);
        for(unsigned i=0;i<3;i++) normal[i]=(u32)((s32)gte_mfc2(25+i)>>shift);
        gte_mtc2(8,(u32)(s32)a[3]);
        for(unsigned i=0;i<3;i++) gte_mtc2(9+i,normal[i]);
        gte_cmd(0x198003d);
        for(unsigned i=0;i<3;i++) normal[i]=gte_mfc2(9+i);
        if(!segment) {
            for(unsigned i=0;i<3;i++) gte_ctc2(i*2,c[12+i]);
            for(unsigned side=0;side<2;side++) {
                u32 x=side ? c[17]-normal[0] : c[17]+normal[0];
                u32 y=side ? c[18]-normal[1] : c[18]+normal[1];
                u32 z=side ? c[19]-normal[2] : c[19]+normal[2];
                gte_mtc2(side*2,(x&65535)|(y<<16));gte_mtc2(side*2+1,z);
            }
            gte_cmd(0x280030);
            *(u32 *)(out+8)=gte_mfc2(12);*(u32 *)(out+20)=gte_mfc2(13);
        } else {
            *(u32 *)(out+8)=c[15];*(u32 *)(out+20)=c[16];
        }
        u32 positive_xy=((c[20]+normal[0])&65535)|((c[21]+normal[1])<<16);
        u32 positive_z=c[22]+normal[2];
        u32 negative_xy=((c[20]-normal[0])&65535)|((c[21]-normal[1])<<16);
        u32 negative_z=c[22]-normal[2];
        if(segment+1>=count) fade=4096;
        gte_mtc2(8,fade);gte_mtc2(6,color);gte_cmd(0x780010);
        *(u32 *)(out+4)=previous_color;*(u32 *)(out+16)=previous_color;
        *(u32 *)(out+12)=uv|*(u16 *)((u8 *)c+36)|c[7];
        *(u32 *)(out+24)=uv|*(u16 *)((u8 *)c+38)|c[6];
        uv=uvbase+((fade*(u32)*(u16 *)((u8 *)c+22))>>12);
        previous_color=gte_mfc2(22);
        for(unsigned i=0;i<3;i++) gte_ctc2(i*2,c[12+i]);
        gte_mtc2(0,positive_xy);gte_mtc2(1,positive_z);
        gte_mtc2(2,negative_xy);gte_mtc2(3,negative_z);
        vertex(4,ring+index*8);
        gte_cmd(0x280030);
        *(u32 *)(out+28)=previous_color;*(u32 *)(out+40)=previous_color;
        *(u16 *)(out+36)=(u16)(uv|*(u16 *)((u8 *)c+36));
        *(u16 *)(out+48)=(u16)(uv|*(u16 *)((u8 *)c+38));
        *(u32 *)(out+32)=c[15]=gte_mfc2(12);
        *(u32 *)(out+44)=c[16]=gte_mfc2(13);
        index=next;
        u32 cull=gte_mfc2(24),depth=gte_mfc2(19);
        u32 bucket=(u32)((s32)((u32)((s32)depth>>2)-c[0])>>2);
        if(cull && bucket<4096) {
            u32 previous=ot[bucket];*(u32 *)out=0x0c000000|(previous&0xffffff);
            ot[bucket]=(previous&0xff000000)|((u32)out&0xffffff);
        }
        fade+=c[8];
    }
}
void o0b_func_800D5C30(s32 count,void *context,u8 *out,u32 *ot) {
    u32 *c=context;
    const u8 *ring=(const u8 *)c[1];
    u32 color=c[3],fade=c[2],previous_color=color;
    u32 uvbase=*(u16 *)((u8 *)c+20),uv=uvbase;
    unsigned size=ring[0]+1,index=ring[1];ring+=4;
    gte_ctc2(21,(c[4]&255)<<4);
    gte_ctc2(22,(c[4]>>4)&0xff0);
    gte_ctc2(23,(c[4]>>12)&0xff0);
    for(unsigned i=0;i<8;i++) c[10+i]=gte_cfc2(i);
    for(s32 segment=0;segment<count;segment++,out+=52) {
        unsigned next=(index+1==size)?0:index+1;
        unsigned after=(next+1==size)?0:next+1;
        vertex(2,ring+index*8);
        vertex(0,ring+(segment>=count-1?next:after)*8);
        vertex(4,ring+next*8);
        gte_cmd(0x280030);
        c[20]=gte_mfc2(14);c[21]=gte_mfc2(13);c[22]=gte_mfc2(12);
        for(unsigned i=0;i<3;i++) c[24+i]=gte_mfc2(25+i);
        s32 x=(s16)c[21],y=(s16)(c[21]>>16);
        c[23]=gte_mfc2(18);
        gte_ctc2(0,0);gte_ctc2(2,0);gte_ctc2(4,4096);
        gte_mtc2(9,(u32)((s32)(s16)c[22]-x)<<7);
        gte_mtc2(10,(u32)((s32)(s16)(c[22]>>16)-y)<<7);
        gte_mtc2(11,0);gte_cmd(0x178000c);
        *(u16 *)((u8 *)c+84)=(u16)(x-(s16)c[20]);
        *(u16 *)((u8 *)c+86)=(u16)(y-(s16)(c[20]>>16));
        u32 normal_x=gte_mfc2(25),normal_y=gte_mfc2(26);
        gte_cmd(0xa00428);
        u32 sum=gte_mfc2(25)+gte_mfc2(26);
        gte_mtc2(30,sum);
        u32 leading=gte_mfc2(31)&~1u;
        unsigned shift=((u32)(31-leading)>>1)&31;
        u32 normalized=leading>=24?sum<<((leading-24)&31):(u32)((s32)sum>>((24-leading)&31));
        u32 offset=(normalized-64)*2;
        s16 reciprocal=*(s16 *)(u32)PSX_ADDR(0x800cfa80+offset);
        gte_mtc2(9,normal_x);gte_mtc2(10,normal_y);gte_mtc2(8,(u32)(s32)reciprocal);
        gte_cmd(0x190003d);
        normal_x=(u32)((s32)gte_mfc2(25)>>shift);
        normal_y=(u32)((s32)gte_mfc2(26)>>shift);
        gte_mtc2(8,(u32)(s32)*(const s16 *)(ring+index*8+6));
        gte_mtc2(9,normal_x);gte_mtc2(10,normal_y);gte_cmd(0x198003d);
        if(segment>=count-1) fade=4096;
        u32 nx=gte_mfc2(9),ny=gte_mfc2(10);
        u32 corner0=(nx+65535)|(ny<<16),corner1=(65535-nx)|((0-ny)<<16);
        *(u32 *)(out+12)=uv|*(u16 *)((u8 *)c+36)|c[7];
        u32 uvstep=(u32)((uint64_t)fade * *(u16 *)((u8 *)c+22));
        for(unsigned i=0;i<3;i++) gte_ctc2(5+i,c[24+i]);
        gte_ctc2(0,4096);gte_ctc2(2,4096);gte_ctc2(4,4096);gte_ctc2(1,0);gte_ctc2(3,0);
        gte_mtc2(0,corner0);gte_mtc2(1,0);gte_mtc2(2,corner1);gte_mtc2(3,0);
        gte_cmd(0x280030);
        *(u32 *)(out+24)=uv|*(u16 *)((u8 *)c+38)|c[6];
        uv=uvbase+(uvstep>>12);
        *(u16 *)(out+36)=(u16)(uv|*(u16 *)((u8 *)c+36));
        *(u16 *)(out+48)=(u16)(uv|*(u16 *)((u8 *)c+38));
        u32 edge0=gte_mfc2(12),edge1=gte_mfc2(13);
        if(!segment) {
            s32 dx=*(s16 *)((u8 *)c+84),dy=*(s16 *)((u8 *)c+86);
            xy_add(out+8,edge0,dx,dy);xy_add(out+20,edge1,dx,dy);
            gte_mtc2(13,edge1);
        } else {
            *(u32 *)(out+8)=c[18];*(u32 *)(out+20)=c[19];
            gte_mtc2(14,c[18]);
        }
        for(unsigned i=0;i<8;i++) gte_ctc2(i,c[10+i]);
        gte_mtc2(8,fade);gte_mtc2(6,color);gte_cmd(0x780010);
        *(u32 *)(out+4)=previous_color;*(u32 *)(out+16)=previous_color;
        previous_color=gte_mfc2(22);
        *(u32 *)(out+28)=previous_color;*(u32 *)(out+40)=previous_color;
        *(u32 *)(out+32)=c[18]=edge0;*(u32 *)(out+44)=c[19]=edge1;
        index=(index+1==size)?0:index+1;
        if(c[23]) {
            gte_cmd(0x1400006);
            if(gte_mfc2(24)) {
                u32 depth=(u32)((s32)((u32)((s32)c[23]>>2)-c[0])>>2);
                if(depth<4096) {
                    u32 old=ot[depth];*(u32 *)out=0x0c000000|(old&0xffffff);
                    ot[depth]=(old&0xff000000)|((u32)out&0xffffff);
                }
            }
        }
        fade+=c[8];
    }
}
