#include "services/video_upload_state.h"
#include "hal/hal_sd.h"
#include <esp_heap_caps.h>
#include <new>
#include <string.h>
#include <stdio.h>
#include <mbedtls/sha256.h>

namespace services { namespace video_upload {
bool binding(const core::secrets::BackendConfig& backend,char (&out)[65]) {
    out[0]=0;
    const size_t urlBytes=strnlen(backend.url,sizeof(backend.url));
    const size_t keyBytes=strnlen(backend.apiKey,sizeof(backend.apiKey));
    if(!urlBytes||urlBytes>=sizeof(backend.url)||!keyBytes||keyBytes>=sizeof(backend.apiKey))return false;
    for(size_t i=0;i<urlBytes;++i)if(uint8_t(backend.url[i])<=32||uint8_t(backend.url[i])>126)return false;
    char key[sizeof(backend.apiKey)];memcpy(key,backend.apiKey,keyBytes+1);
    bool legacy=keyBytes==36;
    for(size_t i=0;i<keyBytes;++i) {
        const char c=key[i];
        if(uint8_t(c)<=32||uint8_t(c)>126)return false;
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')||c=='-'))legacy=false;
    }
    // Match the backend's legacy UUID-shaped key normalization exactly.
    if(legacy)for(size_t i=0;i<keyBytes;++i)if(key[i]>='A'&&key[i]<='F')key[i]+=32;
    static const char domain[]="origami-video-account-v1";
    uint8_t digest[32];mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    const bool ok=mbedtls_sha256_starts_ret(&hash,0)==0&&
        mbedtls_sha256_update_ret(&hash,reinterpret_cast<const uint8_t*>(domain),sizeof(domain))==0&&
        mbedtls_sha256_update_ret(&hash,reinterpret_cast<const uint8_t*>(backend.url),urlBytes+1)==0&&
        mbedtls_sha256_update_ret(&hash,reinterpret_cast<const uint8_t*>(key),keyBytes+1)==0&&
        mbedtls_sha256_finish_ret(&hash,digest)==0;
    mbedtls_sha256_free(&hash);
    if(!ok)return false;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}
    out[64]=0;return true;
}
namespace {
constexpr size_t kBytes=1536,kHeader=16;
constexpr const char* kDirectory="/upload_queue/video";
struct Work { uint8_t bytes[kBytes]; Resume candidate,selected; };
struct Scratch {
    Work* value;
    Scratch():value(static_cast<Work*>(heap_caps_malloc(sizeof(Work),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))) {if(value)new(value) Work;}
    ~Scratch(){if(value){value->~Work();heap_caps_free(value);}}
};
bool pathFor(const char* id,unsigned slot,char* path,size_t capacity) {
    if(!id)return false;
    const size_t n=strnlen(id,24);if(!n||n>=24)return false;
    for(size_t i=0;i<n;++i) {
        const char c=id[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||(c=='.'&&i>0)))return false;
    }
    const int used=snprintf(path,capacity,"%s/%s.%u",kDirectory,id,slot);
    return used>0&&size_t(used)<capacity;
}
uint32_t get32(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put32(uint8_t* p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(i*8));}
uint32_t checksum(const uint8_t* p,size_t n) {
    uint32_t crc=~0u;
    for(size_t i=0;i<n;++i) {
        // Stored checksum bytes are zeroed for the calculation.
        crc^=i>=12&&i<16?0:p[i];
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
bool hash(const char* text,size_t capacity) {
    if(strnlen(text,capacity)!=64)return false;
    for(unsigned i=0;i<64;++i)if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return false;
    return true;
}
bool valid(const Resume& r) {
    using namespace core::video_upload;
    if(!hash(r.backendBinding,sizeof(r.backendBinding))||r.generation>3||
        strnlen(r.session,sizeof(r.session))>=sizeof(r.session)||
        strnlen(r.endpoint,sizeof(r.endpoint))>=sizeof(r.endpoint)||
        strnlen(r.location,sizeof(r.location))>=sizeof(r.location))return false;
    if(!r.media.bytes) {
        // Validate the name with the same contract as a finalized identity.
        Identity nameOnly=r.media;nameOnly.bytes=1;
        memset(nameOnly.sha256,'0',64);nameOnly.sha256[64]=0;
        return !r.media.sha256[0]&&validIdentity(nameOnly)&&!r.session[0]&&!r.generation&&!r.endpoint[0]&&!r.location[0];
    }
    if(!validIdentity(r.media))return false;
    if(!r.session[0])return !r.generation&&!r.endpoint[0]&&!r.location[0];
    if(!core::uploads::validReceiptId(r.session))return false;
    if(!r.endpoint[0])return !r.location[0];
    core::video_upload::detail::Url endpoint;
    if(!core::video_upload::detail::url(r.endpoint,endpoint)||strcmp(endpoint.path,"/storage/v1/upload/resumable/sign"))return false;
    if(!r.location[0])return true;
    // Persist canonical absolute URLs, never relative or signed query URLs.
    core::video_upload::detail::Url location;
    if(!core::video_upload::detail::url(r.location,location)||strcmp(endpoint.host,location.host)||endpoint.port!=location.port)return false;
    const size_t n=strlen(endpoint.path);
    return !strncmp(location.path,endpoint.path,n)&&location.path[n]=='/'&&location.path[n+1];
}
bool compatible(const Resume& old,const Resume& next) {
    if(strcmp(old.media.name,next.media.name)||strcmp(old.backendBinding,next.backendBinding))return false;
    if(old.media.bytes && (old.media.bytes!=next.media.bytes||strcmp(old.media.sha256,next.media.sha256)))return false;
    if(!old.session[0])return true;
    if(strcmp(old.session,next.session)||next.generation<old.generation||next.generation>old.generation+1)return false;
    return next.generation==old.generation||!next.location[0];
}
bool same(const Resume& a,const Resume& b) {
    return a.media.bytes==b.media.bytes&&a.generation==b.generation&&!strcmp(a.media.name,b.media.name)&&
        !strcmp(a.media.sha256,b.media.sha256)&&!strcmp(a.backendBinding,b.backendBinding)&&
        !strcmp(a.session,b.session)&&!strcmp(a.endpoint,b.endpoint)&&!strcmp(a.location,b.location);
}
template<size_t N> bool pack(uint8_t* bytes,size_t& at,const char (&text)[N]) {
    const size_t n=strnlen(text,N);if(n>=N||at+2+n>kBytes)return false;
    bytes[at++]=uint8_t(n);bytes[at++]=uint8_t(n>>8);memcpy(bytes+at,text,n);at+=n;return true;
}
template<size_t N> bool unpack(const uint8_t* bytes,size_t size,size_t& at,char (&text)[N]) {
    if(at+2>size)return false;
    const size_t n=bytes[at]|size_t(bytes[at+1])<<8;at+=2;
    if(n>=N||n>size-at||memchr(bytes+at,0,n))return false;
    memcpy(text,bytes+at,n);text[n]=0;at+=n;return true;
}
size_t encode(const Resume& r,uint32_t sequence,uint8_t* bytes) {
    memset(bytes,0,kHeader);memcpy(bytes,"OVR1",4);put32(bytes+4,sequence);
    size_t at=kHeader;
    put32(bytes+at,r.media.bytes);at+=4;bytes[at++]=r.generation;
    if(!pack(bytes,at,r.media.name)||!pack(bytes,at,r.media.sha256)||!pack(bytes,at,r.backendBinding)||
        !pack(bytes,at,r.session)||!pack(bytes,at,r.endpoint)||!pack(bytes,at,r.location))return 0;
    put32(bytes+8,uint32_t(at));put32(bytes+12,checksum(bytes,at));return at;
}
bool decode(const uint8_t* bytes,size_t size,Resume& r,uint32_t& sequence) {
    if(size<kHeader+5||size>kBytes||memcmp(bytes,"OVR1",4)||get32(bytes+8)!=size||get32(bytes+12)!=checksum(bytes,size))return false;
    size_t at=kHeader;r.media.bytes=get32(bytes+at);at+=4;r.generation=bytes[at++];
    if(!unpack(bytes,size,at,r.media.name)||!unpack(bytes,size,at,r.media.sha256)||!unpack(bytes,size,at,r.backendBinding)||
        !unpack(bytes,size,at,r.session)||!unpack(bytes,size,at,r.endpoint)||!unpack(bytes,size,at,r.location)||at!=size||!valid(r))return false;
    sequence=get32(bytes+4);return true;
}
enum class Read { Valid, Missing, Corrupt, Unavailable };
Read read(const char* path,Work& work,uint32_t& sequence) {
    if(!hal::sd::exists(path))return Read::Missing;
    hal::sd::StreamReader reader;
    if(!hal::sd::openStream(path,&reader))return Read::Unavailable;
    size_t got=0;
    const size_t size=reader.size;
    if(size>=kHeader+5&&size<=kBytes) {
        while(got<size) {
            const size_t n=hal::sd::readStream(&reader,work.bytes+got,size-got);
            if(!n)break;got+=n;
        }
    }
    hal::sd::closeStream(&reader);
    if(size>=kHeader+5&&size<=kBytes&&got!=size)return Read::Unavailable;
    if(size>kBytes)return Read::Corrupt;
    return decode(work.bytes,size,work.candidate,sequence)?Read::Valid:Read::Corrupt;
}
Load loadSlots(const char* id,Resume& out,uint32_t& sequence,unsigned& selected,Work& work) {
    char path[64];if(!pathFor(id,0,path,sizeof(path)))return Load::InvalidId;
    if(!hal::sd::mounted())return Load::Unavailable;
    bool found=false,corrupt=false;
    for(unsigned slot=0;slot<2;++slot) {
        pathFor(id,slot,path,sizeof(path));uint32_t current=0;
        const Read result=read(path,work,current);
        // An unreadable slot may be newer: do not silently roll back through I/O failure.
        if(result==Read::Unavailable)return Load::Unavailable;
        if(result==Read::Corrupt){corrupt=true;continue;}
        if(result!=Read::Valid)continue;
        const uint32_t difference=current-sequence;
        if(found&&(difference==0x80000000u||(difference==0&&!same(out,work.candidate))))return Load::Corrupt;
        if(!found||(difference&&difference<0x80000000u)) {
            out=work.candidate;sequence=current;selected=slot;found=true;
        }
    }
    return found?Load::Ready:corrupt?Load::Corrupt:Load::Missing;
}
}
Identify identify(const char* path,Resume& record,bool (*checkpoint)(void*),void* context) {
    if(!path||!checkpoint||!valid(record)||strnlen(path,96)>=96||path[0]!='/'||
        strstr(path,"//")||strstr(path,"/../")||strstr(path,"/./"))return Identify::Invalid;
    const char* name=strrchr(path,'/');
    if(!name||strcmp(name+1,record.media.name))return Identify::Invalid;
    if(!checkpoint(context))return Identify::Cancelled;
    size_t bytes=0;uint32_t modified=0;
    if(!hal::sd::mounted()||!hal::sd::fileInfo(path,bytes,modified))return Identify::Unavailable;
    if(!bytes||bytes>core::video_upload::kMaximumBytes)return Identify::Invalid;
    if(record.media.bytes&&record.media.bytes!=bytes)return Identify::Changed;
    struct HashWork {
        hal::sd::StreamReader reader;
        mbedtls_sha256_context hash;
        uint8_t buffer[1024],digest[32];
        HashWork(){mbedtls_sha256_init(&hash);}
        ~HashWork(){hal::sd::closeStream(&reader);mbedtls_sha256_free(&hash);}
    };
    struct Owned {
        HashWork* value;
        Owned():value(static_cast<HashWork*>(heap_caps_malloc(sizeof(HashWork),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))){if(value)new(value) HashWork;}
        ~Owned(){if(value){value->~HashWork();heap_caps_free(value);}}
    } owned;
    if(!owned.value)return Identify::NoMemory;
    HashWork& work=*owned.value;
    if(!hal::sd::openStream(path,&work.reader))return Identify::Unavailable;
    if(work.reader.size!=bytes)return Identify::Changed;
    if(mbedtls_sha256_starts_ret(&work.hash,0))return Identify::Unavailable;
    size_t at=0;
    while(at<bytes) {
        if(!checkpoint(context))return Identify::Cancelled;
        const size_t remaining=bytes-at;
        const size_t wanted=remaining<sizeof(work.buffer)?remaining:sizeof(work.buffer);
        const size_t got=hal::sd::readStream(&work.reader,work.buffer,wanted);
        if(!got||got>wanted||mbedtls_sha256_update_ret(&work.hash,work.buffer,got))return Identify::Unavailable;
        at+=got;
    }
    if(!checkpoint(context))return Identify::Cancelled;
    size_t finalBytes=0;uint32_t finalModified=0;
    if(!hal::sd::fileInfo(path,finalBytes,finalModified))return Identify::Unavailable;
    if(finalBytes!=bytes||finalModified!=modified)return Identify::Changed;
    if(mbedtls_sha256_finish_ret(&work.hash,work.digest))return Identify::Unavailable;
    char hex[65];static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){hex[i*2]=digits[work.digest[i]>>4];hex[i*2+1]=digits[work.digest[i]&15];}
    hex[64]=0;
    if(record.media.bytes&&strcmp(record.media.sha256,hex))return Identify::Changed;
    record.media.bytes=uint32_t(bytes);memcpy(record.media.sha256,hex,sizeof(hex));
    return Identify::Ready;
}
Load load(const char* id,Resume& out) {
    Scratch work;if(!work.value)return Load::NoMemory;
    uint32_t sequence=0;unsigned slot=0;
    return loadSlots(id,out,sequence,slot,*work.value);
}
Save save(const char* id,const Resume& record) {
    char path[64];if(!pathFor(id,0,path,sizeof(path))||!valid(record))return Save::Invalid;
    Scratch scratch;if(!scratch.value)return Save::NoMemory;
    Work& work=*scratch.value;uint32_t sequence=0;unsigned selected=1;
    const Load status=loadSlots(id,work.selected,sequence,selected,work);
    if(status==Load::Corrupt)return Save::Corrupt;
    if(status!=Load::Ready&&status!=Load::Missing)return Save::Unavailable;
    if(status==Load::Ready&&!compatible(work.selected,record))return Save::Conflict;
    if(status==Load::Ready&&same(work.selected,record))return Save::Saved;
    if(!hal::sd::ensureDir("/upload_queue")||!hal::sd::ensureDir(kDirectory))return Save::Unavailable;
    pathFor(id,1-selected,path,sizeof(path));++sequence;
    const size_t bytes=encode(record,sequence,work.bytes);
    if(!bytes)return Save::Invalid;
    if(!hal::sd::writeFile(path,work.bytes,bytes))return Save::Unavailable;
    uint32_t verified=0;
    if(read(path,work,verified)!=Read::Valid||verified!=sequence||!same(work.candidate,record))return Save::Unavailable;
    return Save::Saved;
}
bool erase(const char* id) {
    char path[64];if(!pathFor(id,0,path,sizeof(path))||!hal::sd::mounted())return false;
    bool ok=true;
    for(unsigned slot=0;slot<2;++slot) {
        pathFor(id,slot,path,sizeof(path));
        if(hal::sd::exists(path)&&!hal::sd::removeFile(path))ok=false;
    }
    return ok;
}
} }
