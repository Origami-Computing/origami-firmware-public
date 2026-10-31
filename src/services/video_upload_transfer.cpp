#include "services/video_upload_transfer.h"
#include "services/tls_trust.h"
#include "hal/hal_sd.h"
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <new>

namespace services { namespace video_upload {
namespace {
using namespace core::video_upload;
struct Work {
    WiFiClientSecure client;
    hal::sd::StreamReader reader;
    TusReply reply;
    Resume saved;
    core::video_upload::detail::Url url;
    char scratch[1024],location[512];
    bool (*checkpoint)(void*);void* context;
    uint32_t started=0,lastRead=0,readIdleMs=15000;bool cancelled=false;
    Work(bool (*check)(void*),void* user):checkpoint(check),context(user){started=millis();}
    ~Work(){client.stop();hal::sd::closeStream(&reader);}
    bool ready(){if(!checkpoint(context)){cancelled=true;return false;}return uint32_t(millis()-started)<180000u;}
    bool send(const void* bytes,size_t length){
        const auto* p=static_cast<const uint8_t*>(bytes);
        while(length){
            if(!ready())return false;
            const size_t amount=length>1024?1024:length;
            const size_t sent=client.write(p,amount);
            if(!sent||sent>amount)return false;
            p+=sent;length-=sent;
        }
        return true;
    }
    bool text(const char* value){return send(value,strlen(value));}
    bool beginRequest(const char* target,const char* method){
        client.stop();reply=TusReply{};
        if(!core::video_upload::detail::url(target,url)||!ready())return false;
        services::tls::useRootBundle(client);
        if(!client.connect(url.host,url.port,5000)||!ready())return false;
        // Pinned WiFiClientSecure uses seconds here, not Stream milliseconds.
        // Its TLS write retry budget remains the 5-second connect timeout.
        if(client.setTimeout(1)<0)return false;
        const int n=snprintf(scratch,sizeof(scratch),"%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n",method,url.path,url.host,unsigned(url.port));
        return n>0&&size_t(n)<sizeof(scratch)&&send(scratch,n);
    }
    bool begin(const char* target,const char* method,const char* token){
        return beginRequest(target,method)&&text("Tus-Resumable: 1.0.0\r\nx-signature: ")&&text(token)&&text("\r\n");
    }
    int readByte(){
        while(ready()){
            if(client.available()) {const int c=client.read();if(c>=0){lastRead=millis();return c;}}
            else if(!client.connected())return -1;
            if(uint32_t(millis()-lastRead)>=readIdleMs)return -1;
            delay(1);
        }
        return -1;
    }
    bool response(){
        lastRead=millis();return readTusReply([&](){return readByte();},reply);
    }
    bool status(){
        lastRead=millis();size_t n=0;
        for(;;){
            const int c=readByte();if(c<0)return false;
            if(c=='\r'){if(readByte()!='\n')return false;break;}
            if(c<32||c>126||n>=sizeof(scratch)-1)return false;
            scratch[n++]=char(c);
        }
        scratch[n]=0;
        if(n<12||(strncmp(scratch,"HTTP/1.1 ",9)&&strncmp(scratch,"HTTP/1.0 ",9))||
           scratch[9]<'2'||scratch[9]>'5'||scratch[10]<'0'||scratch[10]>'9'||
           scratch[11]<'0'||scratch[11]>'9'||(scratch[12]&&scratch[12]!=' '))return false;
        reply.status=(scratch[9]-'0')*100+(scratch[10]-'0')*10+scratch[11]-'0';return true;
    }
};
class JsonMemory final : public ArduinoJson::Allocator {
    size_t used=0;
    struct alignas(max_align_t) Header {size_t size;};
public:
    void* allocate(size_t n) override {
        if(n>16384-used)return nullptr;
        auto* h=static_cast<Header*>(heap_caps_malloc(n+sizeof(Header),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!h)return nullptr;h->size=n;used+=n;return h+1;
    }
    void deallocate(void* p) override {if(p){auto* h=static_cast<Header*>(p)-1;used-=h->size;heap_caps_free(h);}}
    void* reallocate(void* p,size_t n) override {
        if(!p)return allocate(n);
        auto* h=static_cast<Header*>(p)-1;
        // ArduinoJson requires shrinking a string node to succeed. Keep the
        // existing block (and charge its full retained capacity) without allocation.
        if(n<=h->size)return p;
        void* next=allocate(n);if(!next)return nullptr;
        memcpy(next,p,n<h->size?n:h->size);deallocate(p);return next;
    }
};
struct Body {
    char* value=static_cast<char*>(heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    ~Body(){heap_caps_free(value);}
};
struct Owned {
    Work* value;
    Owned(bool (*check)(void*),void* user):value(static_cast<Work*>(heap_caps_malloc(sizeof(Work),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))){if(value)new(value)Work(check,user);}
    ~Owned(){if(value){value->~Work();heap_caps_free(value);}}
};
}
BackendResult request(BackendOperation operation,const core::secrets::BackendRequest& backend,
                      const Resume& record,core::video_upload::Response& output,
                      bool (*checkpoint)(void*),void* context){
    using namespace core::video_upload;
    char fingerprint[65];
    if(!checkpoint||!validIdentity(record.media)||!binding(backend.backend,fingerprint)||
       strnlen(record.backendBinding,sizeof(record.backendBinding))>=sizeof(record.backendBinding)||strcmp(fingerprint,record.backendBinding)||
       !core::video_upload::detail::clean(backend.token,sizeof(backend.token))||
       strnlen(record.session,sizeof(record.session))>=sizeof(record.session)||record.generation>3||
       (record.session[0]&&!core::uploads::validReceiptId(record.session))||
       (operation!=BackendOperation::Admit&&!record.session[0]))return {BackendOutcome::Invalid,0};
    const char* suffix=nullptr;
    switch(operation){
        case BackendOperation::Admit:suffix="";break;
        case BackendOperation::Status:suffix="";break;
        case BackendOperation::Complete:suffix="/complete";break;
        case BackendOperation::Recover:suffix="/recover";break;
        default:return {BackendOutcome::Invalid,0};
    }
    Owned owned(checkpoint,context);if(!owned.value)return {BackendOutcome::NoMemory,0};
    Work& w=*owned.value;
    if(operation==BackendOperation::Complete)w.readIdleMs=100000;
    auto failure=[&](){return BackendResult{w.cancelled?BackendOutcome::Cancelled:BackendOutcome::Retry,w.reply.status};};
    const size_t baseLength=strlen(backend.backend.url);
    const bool slash=backend.backend.url[baseLength-1]=='/';
    const int urlLength=snprintf(w.location,sizeof(w.location),"%.*s/api/device/video/uploads%s%s%s",
        int(baseLength-(slash?1:0)),backend.backend.url,operation==BackendOperation::Admit?"":"/",
        operation==BackendOperation::Admit?"":record.session,suffix);
    if(urlLength<=0||size_t(urlLength)>=sizeof(w.location)||!core::video_upload::detail::url(w.location,w.url))return {BackendOutcome::Invalid,0};
    if(!w.ready())return failure();
    if(!w.beginRequest(w.location,operation==BackendOperation::Status?"GET":"POST")||
       !w.text("Authorization: Bearer ")||!w.text(backend.token)||!w.text("\r\nX-Api-Key: ")||
       !w.text(backend.backend.apiKey)||!w.text("\r\nAccept-Encoding: identity\r\n"))return failure();
    Body body;if(!body.value)return {BackendOutcome::NoMemory,w.reply.status};
    int bodyLength=0;
    if(operation==BackendOperation::Admit)bodyLength=snprintf(body.value,4096,"{\"name\":\"%s\",\"bytes\":%lu,\"sha256\":\"%s\"}",record.media.name,(unsigned long)record.media.bytes,record.media.sha256);
    else if(operation==BackendOperation::Recover)bodyLength=snprintf(body.value,4096,"{\"generation\":%u}",unsigned(record.generation));
    else if(operation==BackendOperation::Complete){memcpy(body.value,"{}",2);bodyLength=2;}
    const int headerLength=snprintf(w.scratch,sizeof(w.scratch),"Content-Type: application/json\r\nContent-Length: %u\r\n\r\n",unsigned(bodyLength));
    if(bodyLength<0||bodyLength>=4096||headerLength<0||size_t(headerLength)>=sizeof(w.scratch)||
       !w.send(w.scratch,headerLength)||!w.send(body.value,bodyLength)||!w.status())return failure();
    if(w.reply.status<200||w.reply.status>=300)return {BackendOutcome::HttpError,w.reply.status};
    size_t bytes=0;
    if(!core::uploads::readReceiptBody<1024>([&](){return w.readByte();},body.value,4096,bytes))return failure();
    struct Reader {
        const char* data;size_t size,pos=0;
        Reader(const char* d,size_t n):data(d),size(n){}
        int read(){return pos<size?static_cast<unsigned char>(data[pos++]):-1;}
        size_t readBytes(char* out,size_t n){const size_t count=n<size-pos?n:size-pos;memcpy(out,data+pos,count);pos+=count;return count;}
    } reader(body.value,bytes);
    JsonMemory memory;JsonDocument doc(&memory);
    const auto parsed=deserializeJson(doc,reader,DeserializationOption::NestingLimit(4));
    if(parsed==DeserializationError::NoMemory)return {BackendOutcome::NoMemory,w.reply.status};
    if(parsed)return failure();
    while(reader.pos<reader.size){const char c=reader.data[reader.pos++];if(c!=' '&&c!='\r'&&c!='\n'&&c!='\t')return failure();}
    if(!parseResponse(doc,record.media,output,record.session[0]?record.session:nullptr,
         (operation==BackendOperation::Recover||operation==BackendOperation::Status||!record.session[0])?-1:int(record.generation)))return failure();
    if((operation==BackendOperation::Recover||operation==BackendOperation::Status)&&(output.generation<record.generation||output.generation>record.generation+1))return failure();
    if(!w.ready())return failure();
    return {BackendOutcome::Reply,w.reply.status};
}
TransferResult transfer(const char* id,const char* path,Resume& record,const core::video_upload::Response& admission,
                        bool (*checkpoint)(void*),void* context){
    using namespace core::video_upload;
    if(!id||!path||!checkpoint||!validIdentity(record.media)||!admission.hasUpload||admission.state!=State::Pending||
       strnlen(record.backendBinding,sizeof(record.backendBinding))>=sizeof(record.backendBinding)||
       strnlen(record.session,sizeof(record.session))>=sizeof(record.session)||
       strnlen(record.endpoint,sizeof(record.endpoint))>=sizeof(record.endpoint)||
       strnlen(record.location,sizeof(record.location))>=sizeof(record.location)||
       strnlen(admission.session,sizeof(admission.session))>=sizeof(admission.session)||
       strnlen(admission.endpoint,sizeof(admission.endpoint))>=sizeof(admission.endpoint)||
       !core::video_upload::detail::clean(admission.token,sizeof(admission.token))||!core::video_upload::detail::clean(admission.object,sizeof(admission.object))||
       strcmp(record.session,admission.session)||record.generation!=admission.generation||strcmp(record.endpoint,admission.endpoint)||
       strnlen(path,96)>=96||!strrchr(path,'/')||strcmp(strrchr(path,'/')+1,record.media.name))return {Transfer::Invalid,0};
    Owned owned(checkpoint,context);if(!owned.value)return {Transfer::NoMemory,0};
    Work& w=*owned.value;
    if(load(id,w.saved)!=Load::Ready)return {Transfer::Storage,0};
    if(w.saved.media.bytes!=record.media.bytes||strcmp(w.saved.media.name,record.media.name)||
       strcmp(w.saved.media.sha256,record.media.sha256)||strcmp(w.saved.backendBinding,record.backendBinding)||
       strcmp(w.saved.session,record.session)||w.saved.generation!=record.generation||
       strcmp(w.saved.endpoint,record.endpoint)||strcmp(w.saved.location,record.location))return {Transfer::Invalid,0};
    auto failure=[&](){return TransferResult{w.cancelled?Transfer::Cancelled:Transfer::Retry,w.reply.status};};
    if(!core::video_upload::detail::url(record.endpoint,w.url)||strcmp(w.url.path,"/storage/v1/upload/resumable/sign"))return {Transfer::Invalid,0};
    if(record.location[0]&&!resumeLocation(record.endpoint,record.location,w.location,sizeof(w.location)))return {Transfer::Invalid,0};
    if(!w.ready())return failure();
    if(!hal::sd::openStream(path,&w.reader)||w.reader.size!=record.media.bytes)return {Transfer::Storage,0};
    if(!record.location[0]){
        if(!w.begin(record.endpoint,"POST",admission.token))return failure();
        size_t encoded=0;
        if(mbedtls_base64_encode(reinterpret_cast<uint8_t*>(w.location),sizeof(w.location),&encoded,
             reinterpret_cast<const uint8_t*>(admission.object),strlen(admission.object)))return {Transfer::Invalid,0};
        const int n=snprintf(w.scratch,sizeof(w.scratch),"Upload-Length: %lu\r\nContent-Length: 0\r\nUpload-Metadata: bucketName bWVkaWE=,objectName %s,contentType dmlkZW8veC1tc3ZpZGVv\r\n\r\n",(unsigned long)record.media.bytes,w.location);
        if(n<0||size_t(n)>=sizeof(w.scratch)||!w.send(w.scratch,n)||!w.response())return failure();
        if(!createdResource(w.reply,record.endpoint,record.media.bytes,w.location,sizeof(w.location)))return failure();
        if(!w.ready())return failure();
        strcpy(record.location,w.location);
        if(save(id,record)!=Save::Saved){record.location[0]=0;return {Transfer::Storage,w.reply.status};}
        return {Transfer::Progress,w.reply.status};
    }
    if(!w.begin(record.location,"HEAD",admission.token)||!w.text("\r\n")||!w.response())return failure();
    uint32_t offset=0;
    if(!resumeOffset(w.reply,record.media.bytes,offset))return failure();
    if(!w.ready())return failure();
    if(offset==record.media.bytes)return {Transfer::Verify,w.reply.status};
    const uint32_t remaining=record.media.bytes-offset,chunk=remaining<kChunkBytes?remaining:kChunkBytes;
    if(!hal::sd::seekStream(&w.reader,offset))return {Transfer::Storage,w.reply.status};
    if(!w.begin(record.location,"PATCH",admission.token))return failure();
    const int n=snprintf(w.scratch,sizeof(w.scratch),"Upload-Offset: %lu\r\nContent-Length: %lu\r\nContent-Type: application/offset+octet-stream\r\n\r\n",(unsigned long)offset,(unsigned long)chunk);
    if(n<0||size_t(n)>=sizeof(w.scratch)||!w.send(w.scratch,n))return failure();
    uint32_t sent=0;
    while(sent<chunk){
        if(!w.ready())return failure();
        const size_t wanted=chunk-sent<sizeof(w.scratch)?chunk-sent:sizeof(w.scratch);
        const size_t got=hal::sd::readStream(&w.reader,reinterpret_cast<uint8_t*>(w.scratch),wanted);
        if(!got||got>wanted)return {Transfer::Storage,w.reply.status};
        if(!w.send(w.scratch,got))return failure();sent+=got;
    }
    if(!w.response()||!acceptedChunk(w.reply,record.media.bytes,offset,chunk))return failure();
    return {Transfer::Progress,w.reply.status};
}
} }
