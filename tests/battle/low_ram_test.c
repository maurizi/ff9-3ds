#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "psx_mem.h"

unsigned char psx_ram[PSX_RAM_SIZE];
typedef struct {
    uint8_t pad0[4], flags, pad5[7];
    int32_t frame;
    uint8_t pad10[2];
    int16_t speed;
} Animation;
static int calls;
static Animation live[3];
static Animation *lookup(int present) { calls++; return present ? live : NULL; }

int main(void) {
    Animation *a=PSX_NZ(lookup(0));
    assert(calls==1 && (void *)a==(void *)psx_ram);
    /* Original KO MIPS stores at 0x2c, 4, 0xc and 0x12, even when
     * animation lookup returned zero. Other low-RAM bytes stay intact. */
    memset(psx_ram,0xa5,64);
    a[2].flags &= ~3;
    a[0].flags |= 3;
    a[0].frame=0;
    a[0].speed=0x1000;
    uint8_t expected[64];memset(expected,0xa5,sizeof expected);
    expected[0x2c]=0xa4;expected[4]=0xa7;
    memset(expected+0xc,0,4);expected[0x12]=0;expected[0x13]=0x10;
    assert(memcmp(psx_ram,expected,sizeof expected)==0);
    a=PSX_NZ(lookup(1));
    assert(calls==2 && a==live);
    a[0].speed=123;
    assert(live[0].speed==123 && memcmp(psx_ram,expected,sizeof expected)==0);
    const Animation *c=live;
    assert(PSX_NZ(c)==c);
    /* Map a missing bone array before adding a nonzero bone index. Mapping
     * only the final &NULL[index] would retain a low native/ITCM address. */
    typedef struct {uint8_t rotation[0x14];int32_t x,y,z;} Bone;
    _Static_assert(sizeof(Bone)==0x20,"bone stride");
    Bone *bones=NULL;
    PSX_NZ(bones)[3].y=0x12345678;
    int32_t height;
    memcpy(&height,psx_ram+3*0x20+0x18,4);
    assert(bones==NULL && height==0x12345678);
    static Bone ready[4];ready[3].y=-123;
    bones=ready;
    assert(PSX_NZ(bones)[3].y==-123);
    puts("PSX low RAM: KO stores, bone array indices, live pointers and single evaluation pass");
}
