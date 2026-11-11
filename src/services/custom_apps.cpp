#include "services/custom_apps.h"
#include "hal/hal_sd.h"
#ifdef ORIGAMI_NATIVE_APPS
#include "services/native_apps.h"
#endif
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <new>
#include <algorithm>

namespace services { namespace custom_apps {
namespace {
namespace disk=hal::app_storage;
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override{return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
    void deallocate(void* p) override{heap_caps_free(p);}
    void* reallocate(void* p,size_t n) override{return heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
};
uint32_t get32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void put32(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8));}
bool hash(const uint8_t* p,size_t n,uint8_t (&out)[32]){
    mbedtls_sha256_context h;mbedtls_sha256_init(&h);
    const bool ok=mbedtls_sha256_starts_ret(&h,0)==0 && mbedtls_sha256_update_ret(&h,p,n)==0 && mbedtls_sha256_finish_ret(&h,out)==0;
    mbedtls_sha256_free(&h);return ok;
}
void hex(const uint8_t (&bytes)[32],char (&out)[65]){const char* digits="0123456789abcdef";
    for(unsigned i=0;i<32;++i){out[i*2]=digits[bytes[i]>>4];out[i*2+1]=digits[bytes[i]&15];}out[64]=0;}
bool text(const char* p,size_t cap){if(!p)return false;const size_t n=strnlen(p,cap);if(!n||n==cap)return false;
    for(size_t i=0;i<n;++i)if(uint8_t(p[i])<32||uint8_t(p[i])>126)return false;return true;}
bool matches(const uint8_t* bytes,size_t size,const char* expected){if(!expected||strlen(expected)!=64)return false;
    uint8_t digest[32];char actual[65];if(!hash(bytes,size,digest))return false;hex(digest,actual);return !strcmp(expected,actual);}
Result fromDisk(disk::Result r){switch(r){case disk::Result::Ok:return Result::Ok;case disk::Result::Missing:return Result::Missing;
    case disk::Result::Unavailable:return Result::Unavailable;case disk::Result::Corrupt:return Result::Corrupt;
    case disk::Result::Cancelled:return Result::Cancelled;case disk::Result::TooLarge:return Result::Corrupt;default:return Result::Io;}}
bool allocate(Package& p,size_t n){p.reset();p.bytes=static_cast<uint8_t*>(heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return p.bytes;}
void path(const Identity& id,const char* leaf,char (&out)[180]){snprintf(out,sizeof(out),"%s/%s/%s",id.origin,id.id,leaf);}
Result readPackage(const Identity& id,unsigned slot,Package& p){char leaf[20],file[180];snprintf(leaf,sizeof(leaf),"code-%u.kapp",slot);path(id,leaf,file);
    if(!allocate(p,kPackageBytes))return Result::NoMemory;return fromDisk(disk::read(id.medium,file,p.bytes,kPackageBytes,p.size));}
struct Record { bool valid=false, exists=false; uint32_t generation=0,slot=0,bytes=0; uint8_t digest[32]={}; };
constexpr size_t kRecordBytes=80;
Result record(const Identity& id,unsigned index,Record& r){uint8_t b[kRecordBytes],digest[32];size_t n=0;char leaf[20],file[180];
    snprintf(leaf,sizeof(leaf),"active-%u",index);path(id,leaf,file);auto status=disk::read(id.medium,file,b,sizeof(b),n);
    r={};r.exists=status!=disk::Result::Missing;
    if(status==disk::Result::Missing)return Result::Ok;
    if(status!=disk::Result::Ok && status!=disk::Result::TooLarge)return fromDisk(status);
    if(n!=sizeof(b)||memcmp(b,"KAC1",4)||!hash(b,48,digest)||memcmp(digest,b+48,32))return Result::Ok;
    r.generation=get32(b+4);r.slot=get32(b+8);r.bytes=get32(b+12);
    if(!r.generation || r.slot>3 || (r.slot==3?r.bytes!=0:(r.bytes<12||r.bytes>kPackageBytes)))return Result::Ok;
    memcpy(r.digest,b+16,32);r.valid=true;return Result::Ok;
}
Result records(const Identity& id,Record (&r)[2]){auto s=record(id,0,r[0]);if(s!=Result::Ok)return s;return record(id,1,r[1]);}
Result loadRecord(const Identity& id,const Record& r,Package& p){if(!r.valid)return Result::Corrupt;if(r.slot==3)return Result::Missing;
    auto s=readPackage(id,r.slot,p);if(s!=Result::Ok)return s;uint8_t digest[32];View v;
    if(p.size!=r.bytes || !hash(p.bytes,p.size,digest)||memcmp(digest,r.digest,32))return Result::Corrupt;
    s=inspect(p.bytes,p.size,v);if(s!=Result::Ok)return s;return strcmp(v.id,id.id)?Result::Corrupt:Result::Ok;}
unsigned newest(const Record (&r)[2]){return r[1].valid && (!r[0].valid||r[1].generation>r[0].generation)?1:0;}
// Preserve both records' referenced slots, including an incompatible complete
// version. Bounded third slot avoids directory scans or unbounded garbage lists.
Result publish(const Identity& id,const uint8_t* bytes,size_t size,bool tombstone,Cancel cancel){
    if(!validIdentity(id)||id.legacy)return Result::Invalid;
    if(!tombstone){View v;auto s=inspect(bytes,size,v);if(s!=Result::Ok)return s;if(strcmp(id.id,v.id))return Result::Invalid;}
    auto s=fromDisk(disk::mount(id.medium));if(s!=Result::Ok)return s;
    Record r[2];s=records(id,r);if(s!=Result::Ok)return s;
    const unsigned current=newest(r);uint32_t generation=0;
    for(auto& item:r)if(item.valid && item.generation>generation)generation=item.generation;
    if(!tombstone && !generation && (r[0].exists||r[1].exists))return Result::Corrupt;
    if(generation==UINT32_MAX)return Result::Invalid;
    // If current is damaged, keep the validated fallback's record during repair.
    unsigned retained=current;Package check;
    if(!tombstone && r[current].valid && r[current].slot!=3){s=loadRecord(id,r[current],check);
        if(s==Result::Incompatible)return s;
        if(s==Result::NoMemory||s==Result::Io||s==Result::Unavailable)return s;
        if(s!=Result::Ok){
            retained=1-current;s=loadRecord(id,r[retained],check);
            // A valid uninstall record is a recovery point too. Keep it intact
            // while repairing a damaged reinstall; interruption stays uninstalled.
            if(s!=Result::Ok && !(s==Result::Missing && r[retained].valid && r[retained].slot==3))return s;
        }}
    if(!tombstone && retained==current && r[current].valid && r[current].slot!=3 &&
       check.size==size && !memcmp(check.bytes,bytes,size))return cancel&&cancel()?Result::Cancelled:Result::Ok;
    if(tombstone && r[current].valid && r[current].slot==3)return Result::Ok;
    unsigned target=0;while(target<3 && ((r[0].valid&&r[0].slot==target)||(r[1].valid&&r[1].slot==target)))++target;
    char directory[180];snprintf(directory,sizeof(directory),"%s",id.origin);
    if(disk::mkdir(id.medium,directory)!=disk::Result::Ok)return Result::Io;
    snprintf(directory,sizeof(directory),"%s/%s",id.origin,id.id);
    if(disk::mkdir(id.medium,directory)!=disk::Result::Ok)return Result::Io;
    char file[180],leaf[20];
    if(!tombstone){
        snprintf(leaf,sizeof(leaf),"code-%u.kapp",target);path(id,leaf,file);
        auto removed=disk::remove(id.medium,file);if(removed!=disk::Result::Ok && removed!=disk::Result::Missing)return Result::Io;
        uint64_t free=0;if(disk::freeBytes(id.medium,free)!=disk::Result::Ok)return Result::Io;
        // Six erase blocks for record/filesystem metadata in addition to the
        // full replacement. This conservative admission reserve is not a
        // guarantee of space; all write failures still preserve the active code.
        if(free<size+6u*4096u)return Result::NoSpace;
        if(cancel&&cancel())return Result::Cancelled;
        s=fromDisk(disk::write(id.medium,file,bytes,size,cancel));if(s!=Result::Ok)return s;
        s=readPackage(id,target,check);if(s!=Result::Ok)return s;
        if(check.size!=size || memcmp(check.bytes,bytes,size))return Result::Corrupt;
    }
    if(cancel&&cancel())return Result::Cancelled;
    uint8_t b[kRecordBytes]={},digest[32];memcpy(b,"KAC1",4);put32(b+4,generation+1);
    put32(b+8,tombstone?3:target);put32(b+12,tombstone?0:size);
    if(!tombstone){if(!hash(bytes,size,digest))return Result::Io;memcpy(b+16,digest,32);}
    if(!hash(b,48,digest))return Result::Io;memcpy(b+48,digest,32);
    const unsigned destination=generation?1-retained:0;
    snprintf(leaf,sizeof(leaf),"active-%u",destination);path(id,leaf,file);
    // Publication is not cancellable: a short write/readback fault may already
    // have committed. Preserve all package slots on this uncertain outcome.
    if(disk::write(id.medium,file,b,sizeof(b))!=disk::Result::Ok)return Result::Uncertain;
    Record confirmed;s=record(id,destination,confirmed);
    if(s!=Result::Ok||!confirmed.valid||confirmed.generation!=generation+1||
       confirmed.slot!=(tombstone?3:target)||confirmed.bytes!=(tombstone?0:size)||
       memcmp(confirmed.digest,b+16,32))return Result::Uncertain;
    // Cleanup only slots unreferenced by the two retained activation records.
    r[destination]=confirmed;
    for(unsigned i=0;i<3;++i)if((!r[0].valid||r[0].slot!=i)&&(!r[1].valid||r[1].slot!=i)){
        snprintf(leaf,sizeof(leaf),"code-%u.kapp",i);path(id,leaf,file);disk::remove(id.medium,file);
    }
    return Result::Ok;
}
}
Package::~Package(){reset();}
void Package::reset(){heap_caps_free(bytes);bytes=nullptr;size=0;}
const char* resultName(Result r){switch(r){case Result::Ok:return "ok";case Result::Missing:return "missing";case Result::Unavailable:return "unavailable";
    case Result::Corrupt:return "corrupt";case Result::Invalid:return "invalid_package";case Result::Incompatible:return "incompatible";
    case Result::NoMemory:return "no_memory";case Result::NoSpace:return "no_space";case Result::Cancelled:return "cancelled";
    case Result::Network:return "network_error";case Result::Uncertain:return "uncertain";case Result::Busy:return "busy";default:return "io_error";}}
bool validId(const char* id){if(!id)return false;size_t n=strnlen(id,32);if(!n||n>=32)return false;
    for(size_t i=0;i<n;++i)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='0'&&id[i]<='9')||(i>0&&(id[i]=='-'||id[i]=='_'))))return false;
    return true;}
bool validIdentity(const Identity& id){if(id.medium!=Medium::Sd&&id.medium!=Medium::Flash)return false;if(!validId(id.id))return false;
    if(id.legacy)return id.medium==Medium::Sd && !strcmp(id.origin,"local");
    if(!strcmp(id.origin,"local"))return true;if(strnlen(id.origin,65)!=64)return false;
    for(unsigned i=0;i<64;++i)if(!((id.origin[i]>='0'&&id.origin[i]<='9')||(id.origin[i]>='a'&&id.origin[i]<='f')))return false;return true;}
Result inspect(const uint8_t* bytes,size_t size,View& out){out={};if(!bytes||size<12||size>kPackageBytes||memcmp(bytes,"KAPP",4))return Result::Invalid;
    if(get32(bytes+4)!=1)return Result::Incompatible;
    const size_t manifest=get32(bytes+8);if(!manifest||manifest>kManifestBytes||manifest>size-12)return Result::Invalid;
    PsramAllocator allocator;JsonDocument doc(&allocator);
    if(deserializeJson(doc,bytes+12,manifest,DeserializationOption::NestingLimit(4))||!doc.is<JsonObject>())return Result::Invalid;
    const char* id=doc["id"],*name=doc["name"],*version=doc["version"];
    const bool native=!strcmp(doc["kind"]|"","native");
    if(!validId(id)||!text(name,sizeof(out.name))||!text(version,sizeof(out.version))||
       (!native && strcmp(doc["kind"]|"","lua"))||strcmp(doc["entry"]|"",native?"app.elf":"main.lua"))return Result::Invalid;
    if((doc["api"]|0)!=1)return Result::Incompatible;
    if(!doc["source_bytes"].is<unsigned>()||!doc["data_bytes"].is<unsigned>())return Result::Invalid;
    size_t source=doc["source_bytes"].as<unsigned>(),data=doc["data_bytes"].as<unsigned>();
    size_t maximum=kScriptBytes;
    if(native){
#ifdef ORIGAMI_NATIVE_APPS
        maximum=native_apps::kMaxBytes;
        if(!native_apps::verify(id,name,version,doc["native_abi"]|0,doc["source_sha256"]|"",doc["signature"]|"",doc["icon"].isNull()?nullptr:doc["icon"].as<const char*>()))return Result::Incompatible;
#else
        return Result::Incompatible;
#endif
    }
    if(!source||source>maximum||data>kDataBytes||(native&&data)||size-12-manifest!=source+data)return Result::Invalid;
    const uint8_t* script=bytes+12+manifest;const uint8_t* json=script+source;
    if((!native&&(script[0]==0x1b||memchr(script,0,source)))||!matches(script,source,doc["source_sha256"]|"")||!matches(json,data,doc["data_sha256"]|""))return Result::Invalid;
    out.native=native;
    if(!doc["icon"].isNull()){
        const char* icon=doc["icon"];
        if(!native||!icon||strlen(icon)!=100)return Result::Invalid;
        auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
        for(unsigned i=0;i<50;++i){int a=nibble(icon[2*i]),b=nibble(icon[2*i+1]);if(a<0||b<0)return Result::Invalid;out.icon[i]=(a<<4)|b;}
        out.hasIcon=true;
    }
    if(data){JsonDocument input(&allocator);if(deserializeJson(input,json,data,DeserializationOption::NestingLimit(8))||!input.is<JsonObject>())return Result::Invalid;}
    strcpy(out.id,id);strcpy(out.name,name);strcpy(out.version,version);
    out.source=reinterpret_cast<const char*>(script);out.sourceBytes=source;out.data=reinterpret_cast<const char*>(json);out.dataBytes=data;return Result::Ok;
}
Result pack(const char* id,const char* name,const char* version,const char* source,size_t sourceBytes,const char* data,size_t dataBytes,Package& out){
    if(!validId(id)||!text(name,49)||!text(version,32)||!source||!sourceBytes||sourceBytes>kScriptBytes||dataBytes>kDataBytes||(!data&&dataBytes))return Result::Invalid;
    PsramAllocator allocator;JsonDocument doc(&allocator);doc["id"]=id;doc["name"]=name;doc["version"]=version;doc["kind"]="lua";doc["entry"]="main.lua";doc["api"]=1;
    doc["source_bytes"]=sourceBytes;doc["data_bytes"]=dataBytes;
    uint8_t digest[32];char encoded[65];if(!hash(reinterpret_cast<const uint8_t*>(source),sourceBytes,digest))return Result::Io;
    hex(digest,encoded);doc["source_sha256"]=encoded;
    if(!hash(reinterpret_cast<const uint8_t*>(data?data:""),dataBytes,digest))return Result::Io;
    hex(digest,encoded);doc["data_sha256"]=encoded;
    const size_t n=measureJson(doc);if(doc.overflowed()||n>kManifestBytes)return Result::NoMemory;
    if(!allocate(out,12+n+sourceBytes+dataBytes+1))return Result::NoMemory;
    memcpy(out.bytes,"KAPP",4);put32(out.bytes+4,1);put32(out.bytes+8,n);
    if(serializeJson(doc,out.bytes+12,n+1)!=n){out.reset();return Result::Invalid;}
    memcpy(out.bytes+12+n,source,sourceBytes);if(dataBytes)memcpy(out.bytes+12+n+sourceBytes,data,dataBytes);
    out.size=12+n+sourceBytes+dataBytes;View v;return inspect(out.bytes,out.size,v);
}
Result load(const Identity& id,Package& out){out.reset();if(!validIdentity(id))return Result::Invalid;
    if(id.legacy){if(!hal::sd::mounted())return Result::Unavailable;
        Package scratch;if(!allocate(scratch,kScriptBytes+kDataBytes+2))return Result::NoMemory;
        char file[80];snprintf(file,sizeof(file),"/widgets/%s/main.lua",id.id);size_t n=0;uint32_t modified=0;
        if(!hal::sd::fileInfo(file,n,modified))return Result::Missing;if(!n||n>kScriptBytes)return Result::Invalid;
        size_t got=hal::sd::readFile(file,reinterpret_cast<char*>(scratch.bytes),n+1);if(got!=n)return Result::Io;
        snprintf(file,sizeof(file),"/widgets/%s/data.json",id.id);size_t dn=0;
        char* data=reinterpret_cast<char*>(scratch.bytes+kScriptBytes+1);
        if(hal::sd::exists(file)){if(!hal::sd::fileInfo(file,dn,modified)||dn>kDataBytes)return Result::Invalid;
            if(dn && hal::sd::readFile(file,data,dn+1)!=dn)return Result::Io;}
        return pack(id.id,id.id,"legacy",reinterpret_cast<char*>(scratch.bytes),n,data,dn,out);
    }
    auto s=fromDisk(disk::mount(id.medium));if(s!=Result::Ok)return s;Record r[2];s=records(id,r);if(s!=Result::Ok)return s;
    if(!r[0].exists&&!r[1].exists)return Result::Missing;
    unsigned first=newest(r);s=loadRecord(id,r[first],out);
    // Never revive an uninstalled app or silently run an older API after a valid
    // incompatible update. Only damaged/missing package bytes permit fallback.
    if(s==Result::Ok||s==Result::Incompatible||s==Result::NoMemory||s==Result::Io||
       (r[first].valid&&r[first].slot==3))return s;
    return loadRecord(id,r[1-first],out);
}
Result install(const Identity& id,const uint8_t* b,size_t n,Cancel cancel){return publish(id,b,n,false,cancel);}
Result uninstall(const Identity& id,Cancel cancel){return publish(id,nullptr,0,true,cancel);}
bool accountOrigin(const char* url,const char* key,char (&out)[65]){
    if(!url||!key||!url[0]||!key[0])return false;
    mbedtls_sha256_context h;uint8_t digest[32];mbedtls_sha256_init(&h);
    static const char domain[]="kami-custom-apps-v1";
    bool ok=mbedtls_sha256_starts_ret(&h,0)==0&&mbedtls_sha256_update_ret(&h,reinterpret_cast<const uint8_t*>(domain),sizeof(domain))==0&&
        mbedtls_sha256_update_ret(&h,reinterpret_cast<const uint8_t*>(url),strlen(url)+1)==0&&
        mbedtls_sha256_update_ret(&h,reinterpret_cast<const uint8_t*>(key),strlen(key)+1)==0&&mbedtls_sha256_finish_ret(&h,digest)==0;
    mbedtls_sha256_free(&h);if(ok)hex(digest,out);return ok;
}
Result catalog(Catalog& out){out={};
    constexpr size_t scanEntries=kCatalogEntries+1;
    // Scan a bounded number per origin and bound origins as well. Overflow is
    // surfaced, not hidden. Heap scratch avoids TLS worker-stack expansion.
    Package scratch;if(!allocate(scratch,scanEntries*65+scanEntries*32))return Result::NoMemory;
    char* origins=reinterpret_cast<char*>(scratch.bytes);char* ids=origins+scanEntries*65;
    for(Medium medium:{Medium::Flash,Medium::Sd}){
        if(disk::mount(medium)!=disk::Result::Ok)continue;
        bool overflow=false;auto s=disk::listDirs(medium,"",origins,scanEntries,65,overflow);out.overflow|=overflow;
        if(s!=disk::Result::Ok)continue;
        for(unsigned i=0;i<scanEntries && origins[i*65];++i){
            const char* origin=origins+i*65;
            if(disk::listDirs(medium,origin,ids,scanEntries,32,overflow)!=disk::Result::Ok)continue;
            out.overflow|=overflow;
            for(unsigned j=0;j<scanEntries && ids[j*32];++j){Entry e;e.identity.medium=medium;snprintf(e.identity.origin,65,"%s",origin);snprintf(e.identity.id,32,"%s",ids+j*32);
                // FAT may return uppercase 8.3 names (LOCAL, COUNTER). Managed
                // identities are lowercase; FAT lookup is case-insensitive.
                if(medium==Medium::Sd){
                    for(char* c=e.identity.origin;*c;++c)if(*c>='A'&&*c<='Z')*c+= 'a'-'A';
                    for(char* c=e.identity.id;*c;++c)if(*c>='A'&&*c<='Z')*c+= 'a'-'A';
                }
                if(!validIdentity(e.identity))continue;
                Package p;auto result=load(e.identity,p);if(result==Result::Missing)continue;
                View v;if(result==Result::Ok && inspect(p.bytes,p.size,v)==Result::Ok){snprintf(e.name,sizeof(e.name),"%s",v.name);e.native=v.native;e.hasIcon=v.hasIcon;memcpy(e.icon,v.icon,sizeof(e.icon));}else snprintf(e.name,sizeof(e.name),"%s",e.identity.id);
                if(out.count==kCatalogEntries){out.overflow=true;continue;}out.entries[out.count++]=e;
            }
        }
    }
    if(hal::sd::mounted()){
        int count=hal::sd::listDirs("/widgets",ids,scanEntries,32);
        for(int i=0;i<count;++i){Entry e;strcpy(e.identity.id,ids+i*32);e.identity.legacy=true;
            if(!validIdentity(e.identity))continue;snprintf(e.name,sizeof(e.name),"%s",e.identity.id);
            if(out.count==kCatalogEntries){out.overflow=true;continue;}out.entries[out.count++]=e;}
    }
    std::sort(out.entries,out.entries+out.count,[](const Entry& a,const Entry& b){
        int order=strcmp(a.name,b.name);if(order)return order<0;
        if(a.identity.medium!=b.identity.medium)return a.identity.medium==Medium::Flash;
        order=strcmp(a.identity.origin,b.identity.origin);if(order)return order<0;
        order=strcmp(a.identity.id,b.identity.id);if(order)return order<0;
        return a.identity.legacy<b.identity.legacy;
    });
    return Result::Ok;
}
} }
