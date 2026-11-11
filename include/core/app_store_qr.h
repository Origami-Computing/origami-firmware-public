#pragma once
#include <stddef.h>
#include <string.h>
#include "config/backend_host.h"

namespace core { namespace app_store {
// QR payload is bounded by the existing decoder's 256-byte buffer. Only HTTPS,
// no credentials, redirects or fragments other than the required package hash.
struct Download { char url[192]={}; char sha256[65]={}; char code[7]={}; };
inline bool parse(const char* text, Download& out) {
    if(text && strnlen(text,7)==6) {
        for(size_t i=0;i<6;++i)if(text[i]<'0'||text[i]>'9')return false;
        Download parsed;memcpy(parsed.code,text,6);out=parsed;return true;
    }
    if(!text || strnlen(text,256)>=256 || strncmp(text,"origami-app:https://",20))return false;
    const char* url=text+12;const char* fragment=strchr(url,'#');
    if(!fragment || strncmp(fragment,"#sha256=",8) || strlen(fragment+8)!=64)return false;
    const size_t n=fragment-url;if(n>=sizeof(out.url))return false;
    const char* host=url+8;const char* path=strchr(host,'/');
    if(!path || path>=fragment || path==host)return false;
    for(const char* p=host;p<path;++p)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'))return false;
    for(const char* p=path;p<fragment;++p)if(*p<=32||*p>=127||*p=='\\')return false;
    for(const char* p=fragment+8;*p;++p)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')))return false;
    Download parsed;memcpy(parsed.url,url,n);memcpy(parsed.sha256,fragment+8,64);out=parsed;return true;
}
// Protocol 2 pins package bytes while retaining the original URL-scoped install
// identity. Only our exact builder endpoint and UUID query have this meaning;
// arbitrary origins, paths and query parameters remain distinct identities.
inline bool identityUrl(const char* url, char (&out)[192]) {
    if(!url)return false;const size_t n=strnlen(url,sizeof(out));
    if(!n||n>=sizeof(out))return false;
    memcpy(out,url,n+1);
    static const char prefix[]=KAMI_APP_STORE_URL "/api/app-store/builder-";
    if(strncmp(url,prefix,sizeof(prefix)-1))return true;
    const char* code=url+sizeof(prefix)-1;
    if(strlen(code)<6||code[0]<'2'||code[0]>'9')return true;
    for(unsigned i=1;i<6;++i)if(code[i]<'0'||code[i]>'9')return true;
    static const char suffix[]="/package.kapp?v=";
    if(strncmp(code+6,suffix,sizeof(suffix)-1))return true;
    const char* version=code+6+sizeof(suffix)-1;
    if(strlen(version)!=36)return true;
    for(unsigned i=0;i<36;++i){
        if(i==8||i==13||i==18||i==23){if(version[i]!='-')return true;}
        else if(!((version[i]>='0'&&version[i]<='9')||(version[i]>='a'&&version[i]<='f')||(version[i]>='A'&&version[i]<='F')))return true;
    }
    out[(code+6+sizeof("/package.kapp")-1)-url]=0;
    return true;
}

} }
