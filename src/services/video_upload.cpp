#include "services/video_upload.h"
#include "services/video_upload_transfer.h"
#include <esp_heap_caps.h>
#include <new>

namespace services { namespace video_upload {
namespace {
using core::video_upload::State;
struct Work {
    Resume record;
    core::video_upload::Response response;
    core::secrets::BackendRequest backend;
    bool (*checkpoint)(void*);void* context;
    Work(bool (*check)(void*),void* user):backend(core::secrets::backendRequest()),checkpoint(check),context(user){}
    bool ready(){
        if(!checkpoint(context))return false;
        // Do not continue a multi-request turn after credentials changed. The
        // next turn obtains a new coherent snapshot and rechecks account binding.
        return core::secrets::backendRequest().revision==backend.revision;
    }
    static bool check(void* value){auto& w=*static_cast<Work*>(value);return w.checkpoint(w.context);}
    BackendResult call(BackendOperation op){
        if(!ready())return {BackendOutcome::Cancelled,0};
        const auto result=request(op,backend,record,response,check,this);
        return ready()?result:BackendResult{BackendOutcome::Cancelled,result.httpStatus};
    }
};
struct Owned {
    Work* value;
    Owned(bool (*check)(void*),void* user):value(static_cast<Work*>(heap_caps_malloc(sizeof(Work),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))){if(value)new(value)Work(check,user);}
    ~Owned(){if(value){value->~Work();heap_caps_free(value);}}
};
Result progress(uint32_t wait=250){return {Step::Progress,0,wait,"Video progress saved"};}
Result deferred(const char* reason){return {Step::Deferred,0,5000,reason};}
Result rejected(const char* reason){return {Step::Rejected,0,0,reason};}
Result cancelled(){return {Step::Cancelled,0,0,"Video paused"};}
Result backendFailure(Work& w,const BackendResult& result){
    if(result.outcome==BackendOutcome::Cancelled)return cancelled();
    if(result.outcome==BackendOutcome::NoMemory)return deferred("Video memory unavailable");
    if(result.outcome==BackendOutcome::Invalid)return rejected("Video binding invalid");
    if(result.outcome==BackendOutcome::HttpError){
        if(result.httpStatus==401){
            core::secrets::setDeviceTokenFor(w.backend,"");
            return {Step::Deferred,401,60000,"Device registration required"};
        }
        if(result.httpStatus==403)return {Step::Deferred,403,60000,"Video account unavailable"};
        if(result.httpStatus==404)return rejected("Video session unavailable");
        if(result.httpStatus==400)return rejected("Video request rejected");
    }
    return {Step::Retry,result.httpStatus,0,"Video backend retry"};
}
Result finishReply(Work& w){
    if(w.response.state==State::Complete)return {Step::Complete,200,0,"Video receipt verified"};
    return progress(5000);
}
bool saveResponse(const char* id,Work& w){
    if(w.response.generation!=w.record.generation) {
        w.record.location[0]=0;w.record.endpoint[0]=0;
    }
    strcpy(w.record.session,w.response.session);w.record.generation=w.response.generation;
    if(w.response.hasUpload)strcpy(w.record.endpoint,w.response.endpoint);
    return save(id,w.record)==Save::Saved;
}
}
Result advance(const char* id,const char* path,bool (*checkpoint)(void*),void* context){
    if(!id||!path||!checkpoint)return rejected("Video request invalid");
    Owned owned(checkpoint,context);if(!owned.value)return deferred("Video memory unavailable");
    Work& w=*owned.value;
    if(!w.ready())return cancelled();
    const Load loaded=load(id,w.record);
    if(loaded==Load::Missing||loaded==Load::Corrupt||loaded==Load::InvalidId)return rejected("Video resume state missing or corrupt");
    if(loaded!=Load::Ready)return deferred("Video storage unavailable");
    char fingerprint[65];
    if(!binding(w.backend.backend,fingerprint))return {Step::Deferred,0,60000,"Backend account required"};
    if(strcmp(fingerprint,w.record.backendBinding))return rejected("Video account changed; requeue required");
    if(!w.record.media.bytes){
        const Identify identified=identify(path,w.record,Work::check,&w);
        if(identified==Identify::Cancelled)return cancelled();
        if(identified==Identify::Invalid||identified==Identify::Changed)return rejected("Video source changed or invalid");
        if(identified!=Identify::Ready)return deferred("Video identity unavailable");
        if(!w.ready())return cancelled();
        if(save(id,w.record)!=Save::Saved)return deferred("Video identity not saved");
        return progress();
    }
    if(!w.backend.token[0])return {Step::Deferred,0,60000,"Device registration required"};
    if(!w.record.session[0]){
        const auto result=w.call(BackendOperation::Admit);
        if(result.outcome!=BackendOutcome::Reply)return backendFailure(w,result);
        if(w.response.state==State::Complete)return finishReply(w);
        if(!w.ready())return cancelled();
        if(!saveResponse(id,w))return deferred("Video session not saved");
        return progress();
    }
    auto result=w.call(BackendOperation::Status);
    if(result.outcome!=BackendOutcome::Reply)return backendFailure(w,result);
    if(w.response.generation!=w.record.generation){
        // Recover a lost recovery reply without reusing an old object's URL.
        if(!w.ready())return cancelled();
        if(!saveResponse(id,w))return deferred("Video generation not saved");
        return progress();
    }
    if(w.response.state==State::Complete||w.response.state==State::Verifying)return finishReply(w);
    if(w.response.state==State::Failed){
        if(w.record.generation>=3)return rejected("Video recovery limit reached");
        result=w.call(BackendOperation::Recover);
        if(result.outcome!=BackendOutcome::Reply)return backendFailure(w,result);
        if(w.response.state==State::Complete)return finishReply(w);
        if(w.response.generation==w.record.generation)return {Step::Retry,0,0,"Video recovery pending"};
        if(!w.ready())return cancelled();
        if(!saveResponse(id,w))return deferred("Video recovery not saved");
        return progress();
    }
    // Renew only scoped credentials, retaining the exact saved session identity.
    result=w.call(BackendOperation::Admit);
    if(result.outcome!=BackendOutcome::Reply)return backendFailure(w,result);
    if(w.response.state==State::Complete||w.response.state==State::Verifying)return finishReply(w);
    if(w.response.state!=State::Pending||!w.response.hasUpload)return {Step::Retry,0,0,"Video credentials unavailable"};
    if(!w.record.endpoint[0]){
        if(!w.ready())return cancelled();
        if(!saveResponse(id,w))return deferred("Video endpoint not saved");
    } else if(strcmp(w.record.endpoint,w.response.endpoint))return rejected("Video storage destination changed");
    if(!w.ready())return cancelled();
    const auto sent=transfer(id,path,w.record,w.response,Work::check,&w);
    if(!w.ready())return cancelled();
    if(sent.outcome==Transfer::Cancelled)return cancelled();
    if(sent.outcome==Transfer::NoMemory)return deferred("Video memory unavailable");
    if(sent.outcome==Transfer::Storage)return deferred("Video storage unavailable");
    if(sent.outcome==Transfer::Invalid)return rejected("Video transfer state invalid");
    if(sent.outcome==Transfer::Progress)return progress();
    const bool expired=sent.httpStatus==404||sent.httpStatus==410;
    const bool possiblyStored=expired||sent.httpStatus==409||sent.httpStatus==400;
    if(sent.outcome!=Transfer::Verify&&!possiblyStored)return {Step::Retry,sent.httpStatus,0,"Video storage retry"};
    // Provider resource expiry/conflict can also mean the object already exists.
    // Ask the backend to verify before allocating another resource URL.
    result=w.call(BackendOperation::Complete);
    if(result.outcome==BackendOutcome::Reply&&
       (w.response.state==State::Complete||w.response.state==State::Verifying))return finishReply(w);
    if(expired&&result.outcome!=BackendOutcome::Cancelled&&w.ready()){
        w.record.location[0]=0;
        if(save(id,w.record)!=Save::Saved)return deferred("Expired video URL not cleared");
    }
    if(result.outcome!=BackendOutcome::Reply)return backendFailure(w,result);
    return progress(5000);
}
} }
