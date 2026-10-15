#include <assert.h>
#include <string.h>
#include "psx_mem.h"
unsigned char psx_ram[PSX_RAM_SIZE];
char *func_80063108(char *,const char *);
char *func_80065238(char *,const char *);
int main(void) {
    char dst[32];memset(dst,0x5a,sizeof dst);memcpy(dst,"12",3);
    assert(func_80063108(dst," gil")==dst && !strcmp(dst,"12 gil") && dst[7]==0x5a);
    assert(func_80063108(dst,"")==dst && !strcmp(dst,"12 gil"));
    assert(!func_80063108(dst,0) && !strcmp(dst,"12 gil"));
    assert(!func_80063108(0,"x"));
    const unsigned char suffix[]={0xff,0x80,0};
    dst[0]=(char)0xfe;dst[1]=0;
    func_80063108(dst,(const char *)suffix);
    const unsigned char want[]={0xfe,0xff,0x80,0};
    assert(!memcmp(dst,want,sizeof want));
    memcpy(psx_ram+32,"ab",3);memcpy(psx_ram+64,"cd",3);
    assert(func_80063108((char *)32,(char *)64)==(char *)32);
    assert(!strcmp((char *)psx_ram+32,"abcd"));
    assert(func_80065238((char *)32,(char *)64)==(char *)32);
    assert(!strcmp((char *)psx_ram+32,"cd"));
    assert(!func_80065238(dst,0) && !memcmp(dst,want,sizeof want));
    assert(!func_80065238(0,"x"));
    return 0;
}
