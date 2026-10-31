#pragma once
#include "core/upload_receipt.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

namespace core { namespace video_upload {
constexpr uint32_t kMaximumBytes=256u*1024u*1024u;
constexpr uint32_t kChunkBytes=6u*1024u*1024u;
struct Identity { char name[129]{}; uint32_t bytes=0; char sha256[65]{}; };
enum class State { Pending, Verifying, Complete, Failed };
// Caller-owned storage (about 3 KiB); allocate outside the upload task stack.
// No URLs or signed tokens from this structure belong in diagnostic output.
struct Response {
    char session[37]{}; State state=State::Pending; uint8_t generation=0;
    char receipt[37]{}; bool hasUpload=false;
    char endpoint[384]{},object[128]{},token[2048]{};
};
inline bool validIdentity(const Identity& value) {
    const size_t n=strnlen(value.name,sizeof(value.name));
    if (n<5 || n>=sizeof(value.name) || strcmp(value.name+n-4,".avi") ||
        !value.bytes || value.bytes>kMaximumBytes || strnlen(value.sha256,sizeof(value.sha256))!=64) return false;
    for(size_t i=0;i<n-4;++i) {
        const char c=value.name[i];const bool alnum=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');
        if (!alnum && (i==0 || (c!='.'&&c!='_'&&c!='-')))return false;
    }
    for(size_t i=0;i<64;++i)if(!((value.sha256[i]>='0'&&value.sha256[i]<='9')||(value.sha256[i]>='a'&&value.sha256[i]<='f')))return false;
    return true;
}
namespace detail {
inline bool equal(JsonVariantConst value,const char* expected) {
    if(!value.is<const char*>())return false;
    const JsonString text=value.as<JsonString>();
    return text.size()==strlen(expected)&&!memcmp(text.c_str(),expected,text.size());
}
inline bool clean(const char* value,size_t capacity) {
    if(!value)return false;
    const size_t n=strnlen(value,capacity);if(!n||n>=capacity)return false;
    for(size_t i=0;i<n;++i)if(static_cast<unsigned char>(value[i])<33 || static_cast<unsigned char>(value[i])>126)return false;
    return true;
}
template<size_t N> bool copy(JsonVariantConst value,char (&out)[N]) {
    if(!value.is<const char*>())return false;
    const char* s=value.as<const char*>();if(!clean(s,N)||value.as<JsonString>().size()!=strlen(s))return false;
    strcpy(out,s);return true;
}
// HTTPS DNS/IPv4 authorities only, matching the device's current network stack.
// Userinfo, escaped hosts, queries, fragments and dot-segment paths are rejected.
struct Url { char host[192]{}; uint16_t port=443; const char* path=nullptr; };
inline bool url(const char* text,Url& out) {
    if(!clean(text,512)||strncmp(text,"https://",8))return false;
    const char* authority=text+8;const char* path=strchr(authority,'/');
    if(!path||path==authority||size_t(path-authority)>=sizeof(out.host))return false;
    const char* colon=nullptr;
    for(const char* p=authority;p<path;++p) {
        if(*p==':') {if(colon)return false;colon=p;continue;}
        if(colon) {if(*p<'0'||*p>'9')return false;}
        else if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'))return false;
    }
    const size_t hostBytes=(colon?colon:path)-authority;
    if(!hostBytes)return false;
    for(size_t i=0;i<hostBytes;++i)out.host[i]=authority[i]>='A'&&authority[i]<='Z'?authority[i]+32:authority[i];
    out.host[hostBytes]=0;out.port=443;
    if(colon) {
        if(colon+1==path)return false;
        uint32_t port=0;
        for(const char* p=colon+1;p<path;++p) {port=port*10+(*p-'0');if(port>65535)return false;}
        if(!port)return false;out.port=uint16_t(port);
    }
    for(const char* p=path;*p;++p) {
        const char c=*p;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='-'||c=='_'||c=='.'||c=='+'||c=='='))return false;
    }
    if(strstr(path,"//")||strstr(path,"/../")||strstr(path,"/./"))return false;
    const size_t n=strlen(path);
    if((n>=3&&!strcmp(path+n-3,"/.."))||(n>=2&&!strcmp(path+n-2,"/.")))return false;
    out.path=path;return true;
}
}
// Validate an admission/status/completion response against the exact local file.
// JSON framing, parsing limits, allocation and transport remain caller-owned.
// Output is usable only on true. A valid response is not necessarily completion:
// only State::Complete has a verified receipt; hasUpload permits TUS admission.
// Supply the persisted session/generation for status and completion replies.
inline bool parseResponse(JsonVariantConst root,const Identity& expected,Response& out,
                          const char* boundSession=nullptr,int boundGeneration=-1) {
    if(!validIdentity(expected)||!root.is<JsonObjectConst>())return false;
    if(!detail::equal(root["name"],expected.name)||!detail::equal(root["sha256"],expected.sha256)||!root["bytes"].is<uint32_t>()||
        root["bytes"].as<uint32_t>()!=expected.bytes||!root["uploaded"].is<bool>()||
        !root["generation"].is<unsigned>()||root["generation"].as<unsigned>()>3||
        !detail::copy(root["session_id"],out.session)||!core::uploads::validReceiptId(out.session))return false;
    out.generation=uint8_t(root["generation"].as<unsigned>());
    if((boundSession&&strcmp(boundSession,out.session))||boundGeneration < -1||boundGeneration>3||
        (boundGeneration>=0&&unsigned(boundGeneration)!=out.generation))return false;
    if(detail::equal(root["state"],"pending"))out.state=State::Pending;
    else if(detail::equal(root["state"],"verifying"))out.state=State::Verifying;
    else if(detail::equal(root["state"],"complete"))out.state=State::Complete;
    else if(detail::equal(root["state"],"failed"))out.state=State::Failed;
    else return false;
    out.hasUpload=false;out.receipt[0]=out.endpoint[0]=out.object[0]=out.token[0]=0;
    if(root["uploaded"].as<bool>()!=(out.state==State::Complete))return false;
    if(out.state==State::Complete) {
        return detail::equal(root["kind"],"video")&&detail::copy(root["id"],out.receipt)&&core::uploads::validReceiptId(out.receipt)&&root["upload"].isNull();
    }
    if(!root["id"].isNull()||!root["kind"].isNull())return false;
    if(root["upload"].isNull())return true;
    if(out.state!=State::Pending)return false;
    const auto upload=root["upload"];
    if(!detail::equal(upload["bucket"],"media")||!detail::equal(upload["content_type"],"video/x-msvideo")||!upload["chunk_bytes"].is<uint32_t>()||
        upload["chunk_bytes"].as<uint32_t>()!=kChunkBytes||!detail::copy(upload["endpoint"],out.endpoint)||
        !detail::copy(upload["object"],out.object)||!detail::copy(upload["token"],out.token))return false;
    detail::Url endpoint;
    if(!detail::url(out.endpoint,endpoint)||strcmp(endpoint.path,"/storage/v1/upload/resumable/sign"))return false;
    if(strncmp(out.object,"device-video/",13)||strlen(out.object)<50)return false;
    char owner[37];memcpy(owner,out.object+13,36);owner[36]=0;
    if(!core::uploads::validReceiptId(owner)||out.object[49]!='/')return false;
    char suffix[48];
    if(out.generation)snprintf(suffix,sizeof(suffix),"%s-r%u.avi",out.session,unsigned(out.generation));
    else snprintf(suffix,sizeof(suffix),"%s.avi",out.session);
    if(strcmp(out.object+50,suffix))return false;
    out.hasUpload=true;return true;
}
// Scoped signatures require /resumable/sign; the ordinary route requires JWT auth.
// TUS Location is untrusted even after TLS: credentials may only be sent back
// to the admitted storage origin and its resumable-resource subtree. Resolve
// absolute-path references; redirects to other origins are never followed.
// out must not alias either input and is usable only when this returns true.
inline bool resumeLocation(const char* endpoint,const char* location,char* out,size_t capacity) {
    detail::Url base;
    if(!detail::url(endpoint,base)||strcmp(base.path,"/storage/v1/upload/resumable/sign")||!location||!out||!capacity)return false;
    if(location[0]=='/') {
        const size_t originBytes=base.path-endpoint,n=strnlen(location,512);
        if(n>=512||originBytes+n>=capacity)return false;
        memcpy(out,endpoint,originBytes);memcpy(out+originBytes,location,n+1);
    } else {
        const size_t n=strnlen(location,512);if(n>=512||n>=capacity)return false;
        memcpy(out,location,n+1);
    }
    detail::Url resolved;
    if(!detail::url(out,resolved)||strcmp(base.host,resolved.host)||base.port!=resolved.port)return false;
    const size_t prefix=strlen(base.path);
    return !strncmp(resolved.path,base.path,prefix)&&resolved.path[prefix]=='/'&&resolved.path[prefix+1];
}
// Bounded HTTP/1.x response head for the provider's TUS operations. The caller
// supplies deadline/cancellation-aware byte reads and closes after each response;
// this parser neither follows redirects nor consumes an error response body.
struct TusReply {
    unsigned status=0;
    bool version=false,hasOffset=false,hasLength=false,hasContentLength=false,chunked=false;
    uint32_t offset=0,length=0,contentLength=0;
    char location[512]{};
};
template<class ReadByte> bool readTusReply(ReadByte readByte,TusReply& out) {
    out=TusReply{};
    char line[768];size_t total=0;
    auto readLine=[&]() {
        size_t n=0;
        for(;;) {
            const int c=readByte();if(c<0||++total>8192)return false;
            if(c=='\r') {
                if(readByte()!='\n'||++total>8192)return false;
                line[n]=0;return true;
            }
            if(c=='\n'||(c<32&&c!='\t')||c>126||n>=sizeof(line)-1)return false;
            line[n++]=char(c);
        }
    };
    auto number=[](const char* text,uint32_t limit,uint32_t& value) {
        value=0;if(!*text)return false;
        for(;*text;++text) {
            if(*text<'0'||*text>'9')return false;
            const uint32_t digit=*text-'0';
            if(digit>limit||value>(limit-digit)/10)return false;
            value=value*10+digit;
        }
        return true;
    };
    if(!readLine()||strlen(line)<12||
        (strncmp(line,"HTTP/1.1 ",9)&&strncmp(line,"HTTP/1.0 ",9))||
        line[9]<'2'||line[9]>'5'||line[10]<'0'||line[10]>'9'||line[11]<'0'||line[11]>'9'||
        (line[12]&&line[12]!=' '))return false;
    out.status=(line[9]-'0')*100+(line[10]-'0')*10+line[11]-'0';
    bool haveVersion=false,haveLocation=false;
    for(;;) {
        if(!readLine())return false;
        if(!line[0])return true;
        char* colon=strchr(line,':');if(!colon||colon==line)return false;
        for(char* p=line;p<colon;++p)
            if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'))return false;
        *colon=0;char* value=colon+1;
        while(*value==' '||*value=='\t')++value;
        char* end=value+strlen(value);while(end>value&&(end[-1]==' '||end[-1]=='\t'))*--end=0;
        if(!strcasecmp(line,"Tus-Resumable")) {
            if(haveVersion||strcmp(value,"1.0.0"))return false;
            haveVersion=out.version=true;
        } else if(!strcasecmp(line,"Upload-Offset")) {
            if(out.hasOffset||!number(value,kMaximumBytes,out.offset))return false;
            out.hasOffset=true;
        } else if(!strcasecmp(line,"Upload-Length")) {
            if(out.hasLength||!number(value,kMaximumBytes,out.length))return false;
            out.hasLength=true;
        } else if(!strcasecmp(line,"Location")) {
            if(haveLocation||!detail::clean(value,sizeof(out.location)))return false;
            strcpy(out.location,value);haveLocation=true;
        } else if(!strcasecmp(line,"Content-Length")) {
            if(out.hasContentLength||out.chunked||!number(value,UINT32_MAX,out.contentLength))return false;
            out.hasContentLength=true;
        } else if(!strcasecmp(line,"Transfer-Encoding")) {
            if(out.chunked||out.hasContentLength||strcasecmp(value,"chunked"))return false;
            out.chunked=true;
        }
    }
}
// Empty POST creation only: no data is sent until the resource URL is durably
// saved. A subsequent HEAD supplies the authoritative offset after interruption.
inline bool createdResource(const TusReply& reply,const char* endpoint,uint32_t total,char* location,size_t capacity) {
    return total&&total<=kMaximumBytes&&reply.status==201&&reply.version&&
        (!reply.hasOffset||reply.offset==0)&&(!reply.hasLength||reply.length==total)&&
        resumeLocation(endpoint,reply.location,location,capacity);
}
inline bool resumeOffset(const TusReply& reply,uint32_t total,uint32_t& offset) {
    if(!total||total>kMaximumBytes||(reply.status!=200&&reply.status!=204)||!reply.version||
        !reply.hasOffset||!reply.hasLength||reply.length!=total||reply.offset>total)return false;
    offset=reply.offset;return true;
}
// A provider acknowledgement proves this chunk only, even if offset==total.
// Queue completion still requires the separately validated backend video receipt.
inline bool acceptedChunk(const TusReply& reply,uint32_t total,uint32_t offset,uint32_t sent) {
    return total&&total<=kMaximumBytes&&offset<=total&&sent&&sent<=kChunkBytes&&sent<=total-offset&&
        (sent==kChunkBytes||sent==total-offset)&&reply.status==204&&reply.version&&reply.hasOffset&&
        reply.offset==offset+sent&&(!reply.hasLength||reply.length==total)&&
        !reply.chunked&&(!reply.hasContentLength||reply.contentLength==0);
}
} }
