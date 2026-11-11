#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace core { namespace wifi_audio {
// Printable GGWave envelope: W1:<CRC32 hex>:<index>:<total>:<96 base64 chars>.
// At most 512 decoded bytes. CRC identifies a transfer and rejects mixed chunks;
// it detects corruption, not authenticity. The UI owns the receiving session.
struct Assembly {
    char encoded[8][97]{};
    uint32_t checksum=0, lastMs=0;
    unsigned total=0, mask=0;
    void clear() { memset(this,0,sizeof(*this)); }
    unsigned received() const { unsigned n=0;for(unsigned b=mask;b;b>>=1)n+=b&1;return n; }
    static int digit(char c) {
        if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;
        if(c>='0'&&c<='9')return c-'0'+52;return c=='+'?62:c=='/'?63:-1;
    }
    // 0=incomplete, 1=complete, -1=invalid; completed output is caller-owned.
    int add(const char* packet,uint32_t now,char* out,size_t capacity) {
        if(total && uint32_t(now-lastMs)>90000)clear();
        const size_t n=strlen(packet);
        if(n<18||strncmp(packet,"W1:",3)||packet[11]!=':'||packet[13]!=':'||packet[15]!=':')return -1;
        uint32_t crc=0;
        for(int i=3;i<11;++i){char c=packet[i];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;if(d<0)return -1;crc=(crc<<4)|d;}
        const unsigned index=packet[12]-'0', count=packet[14]-'0';
        const size_t len=n-16;
        if(!count||count>8||index>=count||len>96||len%4||(index+1<count&&len!=96))return -1;
        bool padding=false;unsigned pads=0;
        for(size_t i=16;i<n;++i){if(packet[i]=='='){padding=true;++pads;}else if(padding||digit(packet[i])<0)return -1;}
        if(pads>2||(pads&&index+1<count)||packet[16]=='='||packet[17]=='=')return -1;
        if(total&&(checksum!=crc||total!=count))clear();
        checksum=crc;total=count;lastMs=now;
        if((mask&(1u<<index)) && strcmp(encoded[index],packet+16)) {clear();return -1;}
        memcpy(encoded[index],packet+16,len+1);mask|=1u<<index;
        if(mask!=(1u<<total)-1)return 0;
        size_t used=0;uint32_t computed=0xffffffff;
        for(unsigned part=0;part<total;++part)for(size_t i=0;encoded[part][i];i+=4){
            const char* p=encoded[part]+i;
            const uint32_t bits=(digit(p[0])<<18)|(digit(p[1])<<12)|((p[2]=='='?0:digit(p[2]))<<6)|(p[3]=='='?0:digit(p[3]));
            const int bytes=p[2]=='='?1:p[3]=='='?2:3;
            for(int b=0;b<bytes;++b){
                if(used+1>=capacity||used>=512){clear();return -1;}
                const uint8_t ch=bits>>(16-8*b);if(!ch){clear();return -1;}
                out[used++]=char(ch);computed^=ch;
                for(int bit=0;bit<8;++bit)computed=(computed>>1)^(0xedb88320u&uint32_t(-int(computed&1)));
            }
        }
        out[used]=0;const bool valid=(computed^0xffffffff)==checksum;clear();
        if(!valid){memset(out,0,capacity);return -1;}return 1;
    }
};
}}
