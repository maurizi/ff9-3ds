#include <assert.h>
#include <string.h>
#include "psx_mem.h"
unsigned char psx_ram[PSX_RAM_SIZE];
char *psx_bios_strcat(char *,const char *);
char *psx_bios_strcpy(char *,const char *);
int main(void) {
    char dst[32];memset(dst,0x5a,sizeof dst);memcpy(dst,"12",3);
    assert(psx_bios_strcat(dst," gil")==dst && !strcmp(dst,"12 gil") && dst[7]==0x5a);
    assert(psx_bios_strcat(dst,"")==dst && !strcmp(dst,"12 gil"));
    assert(!psx_bios_strcat(dst,0) && !strcmp(dst,"12 gil"));
    assert(!psx_bios_strcat(0,"x"));
    const unsigned char suffix[]={0xff,0x80,0};
    dst[0]=(char)0xfe;dst[1]=0;
    psx_bios_strcat(dst,(const char *)suffix);
    const unsigned char want[]={0xfe,0xff,0x80,0};
    assert(!memcmp(dst,want,sizeof want));
    memcpy(psx_ram+32,"ab",3);memcpy(psx_ram+64,"cd",3);
    assert(psx_bios_strcat((char *)32,(char *)64)==(char *)32);
    assert(!strcmp((char *)psx_ram+32,"abcd"));
    assert(psx_bios_strcpy((char *)32,(char *)64)==(char *)32);
    assert(!strcmp((char *)psx_ram+32,"cd"));
    assert(!psx_bios_strcpy(dst,0) && !memcmp(dst,want,sizeof want));
    assert(!psx_bios_strcpy(0,"x"));
    return 0;
}
