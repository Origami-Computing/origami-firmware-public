#ifdef ORIGAMI_NATIVE_APPS
#include "services/native_apps.h"
#endif
#include "widgets/storage.h"
#include "config/backend_host.h"
#include "widgets/sync.h"
#include <atomic>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include <new>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace widgets { namespace storage {
namespace {
using namespace services::custom_apps;
enum class State { Idle, Preparing, Queued, Running, Complete };
std::atomic<State> state{State::Idle};
std::atomic<int> progress{-2}; // Preparing=-2, unknown=-1, percentage=0..100, installing=101.
std::atomic<bool> cancelled{false},discard{false};
uint32_t serial=0,ticket=0;
Operation operation;Identity selected;Package payload;Catalog* listing=nullptr;Result outcome;
core::app_store::Download downloadRequest;
struct DownloadJob { core::app_store::Download source; DownloadTarget target; };
char backendId[80]={},backendSlug[32]={};
bool (*workerPause)()=nullptr;
bool stopping(){return cancelled.load() || (workerPause&&workerPause());}
// Worker-only. Uninstall an app copy only when one exists, so no empty
// tombstone is written onto a medium that never held the app.
void dropIfPresent(const Identity& id){Package existing;if(load(id,existing)==Result::Ok)(void)uninstall(id,stopping);}
// After a store-scoped install, remove copies an earlier install left under a
// different identity: the other storage medium, and the URL-scoped origin that
// earlier firmware derived from this same download URL.
void removeEarlierCopies(const Identity& current,const char* originUrl){
    Identity other=current;
    other.medium=current.medium==Medium::Flash?Medium::Sd:Medium::Flash;
    dropIfPresent(other);
    Identity urlScoped=current;
    if(!accountOrigin(originUrl,"public-app-store",urlScoped.origin)||!strcmp(urlScoped.origin,current.origin))return;
    const Medium media[]={Medium::Flash,Medium::Sd};
    for(Medium medium:media){urlScoped.medium=medium;dropIfPresent(urlScoped);}
}
bool nativePrepared=false;
void cleanup(){
#ifdef ORIGAMI_NATIVE_APPS
    if(nativePrepared)services::native_apps::discardPrepared();
#endif
    nativePrepared=false;
payload.reset();heap_caps_free(listing);listing=nullptr;state.store(State::Idle,std::memory_order_release);}
struct Heap {size_t used=0;};
void* luaAllocate(void* context,void* old,size_t before,size_t after){
    auto& h=*static_cast<Heap*>(context);if(!old)before=0;
    if(!after){h.used-=before;heap_caps_free(old);return nullptr;}
    if(after>128u*1024u || h.used-before>128u*1024u-after)return nullptr;
    void* p=heap_caps_realloc(old,after,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(p)h.used=h.used-before+after;return p;
}
}
Result validateLua(const uint8_t* data,size_t n){View view;auto r=inspect(data,n,view);if(r!=Result::Ok)return r;
    if(view.native)return Result::Ok;
    Heap heap;lua_State* L=lua_newstate(luaAllocate,&heap);if(!L)return Result::NoMemory;
    const int status=luaL_loadbufferx(L,view.source,view.sourceBytes,"=installed-app","t");lua_close(L);
    return status==LUA_OK?Result::Ok:status==LUA_ERRMEM?Result::NoMemory:Result::Invalid;
}
uint32_t request(Operation op,const Identity& id,const uint8_t* bytes,size_t size,const char* remoteId,const char* slug){
    if(size>kPackageBytes || (size&&!bytes) || (remoteId&&strlen(remoteId)>=sizeof(backendId)) || (slug&&strlen(slug)>=sizeof(backendSlug)))return 0;
    State idle=State::Idle;if(!state.compare_exchange_strong(idle,State::Preparing))return 0;
    payload.reset();if(size){payload.bytes=static_cast<uint8_t*>(heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!payload.bytes){cleanup();return 0;}memcpy(payload.bytes,bytes,size);payload.size=size;}
    progress.store(-2);operation=op;selected=id;backendId[0]=backendSlug[0]=0;
    if(remoteId)strcpy(backendId,remoteId);if(slug)strcpy(backendSlug,slug);
    cancelled=false;discard=false;ticket=++serial;if(!ticket)ticket=++serial;
    if(!sync::startPolling()){cleanup();return 0;}
    state.store(State::Queued,std::memory_order_release);sync::requestPoll();return ticket;
}
uint32_t requestDownload(const char* qr,DownloadTarget target){
    if(target!=DownloadTarget::ByKind&&target!=DownloadTarget::Sd&&target!=DownloadTarget::Flash)return 0;
    core::app_store::Download parsed;if(!core::app_store::parse(qr,parsed))return 0;
    // UI submissions are serialized. Publish the descriptor as owned payload
    // before Queued; the worker never borrows the scanner buffer.
    DownloadJob job{parsed,target};
    return request(Operation::Download,Identity{},reinterpret_cast<const uint8_t*>(&job),sizeof(job));
}
bool cancelRequested(){return state.load()==State::Running && cancelled.load();}
void reportDownloadProgress(size_t received,size_t total){
    progress.store(total?int((uint64_t(received>total?total:received)*100)/total):-1);
}
DownloadProgress downloadProgress(uint32_t wanted){
    if(state.load(std::memory_order_acquire)==State::Idle||wanted!=ticket)return {};
    const int value=progress.load();
    if(value==-2)return {};
    if(value==101)return {DownloadProgress::Phase::Installing,100};
    return {DownloadProgress::Phase::Downloading,value};
}
bool pending(){return state.load(std::memory_order_acquire)==State::Queued;}
bool networkPending(){return pending() && (operation==Operation::Sync || operation==Operation::Download);}
void process(bool (*pause)()){
    State queued=State::Queued;if(!state.compare_exchange_strong(queued,State::Running))return;
    workerPause=pause;outcome=Result::Cancelled;
    bool retryDownload=false;
    if(stopping())retryDownload=operation==Operation::Download&&!cancelled.load();
    else switch(operation){
    case Operation::Download:{
        if(payload.size!=sizeof(DownloadJob)){outcome=Result::Invalid;break;}
        DownloadJob download;memcpy(&download,payload.bytes,sizeof(download));
        Package fetched; // Keep the request intact until temporary worker pauses are resolved.
        downloadRequest=download.source;
        outcome=sync::download(downloadRequest,fetched,stopping);
        if(outcome==Result::Ok)progress.store(101);
        View view;
        if(outcome==Result::Ok)outcome=inspect(fetched.bytes,fetched.size,view);
        if(outcome==Result::Ok)outcome=validateLua(fetched.bytes,fetched.size);
        if(outcome==Result::Ok){
            selected.medium=download.target==DownloadTarget::ByKind ? (view.native && hal::app_storage::hasFlashStore()?Medium::Flash:Medium::Sd) :
                download.target==DownloadTarget::Flash?Medium::Flash:Medium::Sd;
            strcpy(selected.id,view.id);
            // Public packages never overwrite local or account apps. A package
            // from Kami's store, or one carrying a valid publisher signature, is
            // one app per id: downloading it again replaces the installed copy,
            // whatever store host, URL or storage medium the earlier copy used.
            // Unsigned packages from other URLs stay URL-scoped.
            char originUrl[192];
            const bool fromStore=!strncmp(downloadRequest.url,KAMI_APP_STORE_URL "/",sizeof(KAMI_APP_STORE_URL));
            const bool storeScoped=view.native||fromStore;
            const bool urlOk=core::app_store::identityUrl(downloadRequest.url,originUrl);
            if(!urlOk||!accountOrigin(storeScoped?"kami-app-store":originUrl,"public-app-store",selected.origin))
                outcome=Result::Invalid;
            else outcome=install(selected,fetched.bytes,fetched.size,stopping);
            if(outcome==Result::Ok && storeScoped)removeEarlierCopies(selected,originUrl);
        }
        retryDownload=outcome==Result::Cancelled&&!cancelled.load();
        if(!retryDownload){payload.reset();payload.bytes=fetched.bytes;payload.size=fetched.size;fetched.bytes=nullptr;fetched.size=0;}
        break;}
    case Operation::PrepareNative:{
        outcome=Result::Incompatible;
#ifdef ORIGAMI_NATIVE_APPS
        View view;outcome=inspect(payload.bytes,payload.size,view);
        if(outcome==Result::Ok){
            if(!view.native)outcome=Result::Invalid;
            else if(services::native_apps::prepare(reinterpret_cast<const uint8_t*>(view.source),view.sourceBytes))nativePrepared=true;
            else outcome=Result::Invalid;
        }
        if(nativePrepared&&stopping())outcome=Result::Cancelled;
#endif
        break;}
    case Operation::Catalog:{
        // Worker-owned result storage; internal RAM is reserved for radio/DMA.
        void* memory=heap_caps_malloc(sizeof(Catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!memory){outcome=Result::NoMemory;break;}
        listing=new(memory) Catalog{};outcome=catalog(*listing);break;}
    case Operation::Load:outcome=load(selected,payload);break;
    case Operation::Install:
        outcome=validateLua(payload.bytes,payload.size);
        if(outcome==Result::Ok)outcome=install(selected,payload.bytes,payload.size,stopping);break;
    case Operation::Import:{Identity legacy=selected;legacy.legacy=true;legacy.medium=Medium::Sd;strcpy(legacy.origin,"local");
        outcome=load(legacy,payload);if(outcome==Result::Ok)outcome=validateLua(payload.bytes,payload.size);
        if(outcome==Result::Ok){selected.legacy=false;outcome=install(selected,payload.bytes,payload.size,stopping);}break;}
    case Operation::Remove:outcome=uninstall(selected,stopping);break;
    case Operation::InitializeFlash:{auto r=hal::app_storage::initializeBlankFlash(stopping);
        outcome=r==hal::app_storage::Result::Ok?Result::Ok:r==hal::app_storage::Result::Unavailable?Result::Unavailable:
            r==hal::app_storage::Result::Cancelled?Result::Cancelled:r==hal::app_storage::Result::Corrupt?Result::Corrupt:Result::Io;break;}
    case Operation::Sync:{char err[48];
        int result=backendId[0]?sync::syncOne(backendId,backendSlug,err,sizeof(err),&selected):sync::syncFromBackend(err,sizeof(err));
        outcome=result<0?Result::Io:Result::Ok;
        if(outcome==Result::Ok&&backendId[0]){
            outcome=load(selected,payload);
        }break;}
    }
    workerPause=nullptr;
    // A shared worker pause is not a user cancellation. Retry only downloads,
    // retaining their owned request; never publish a spurious terminal result.
    if(retryDownload){progress.store(-2);state.store(State::Queued,std::memory_order_release);return;}
    state.store(State::Complete,std::memory_order_release);
    if(discard.load(std::memory_order_acquire)){State done=State::Complete;if(state.compare_exchange_strong(done,State::Preparing))cleanup();}
}
bool take(uint32_t wanted,Result& result,Package* package,Catalog* catalogOut,Identity* identity){
    if(state.load(std::memory_order_acquire)!=State::Complete||wanted!=ticket)return false;
    State done=State::Complete;if(!state.compare_exchange_strong(done,State::Preparing))return false;
    if(operation==Operation::PrepareNative&&outcome==Result::Ok)nativePrepared=false; // transfer to UI activation
    result=outcome;if(package){package->reset();package->bytes=payload.bytes;package->size=payload.size;payload.bytes=nullptr;payload.size=0;}
    if(catalogOut)*catalogOut=listing?*listing:Catalog{};if(identity)*identity=selected;cleanup();return true;
}
void cancel(uint32_t wanted){if(state.load()!=State::Idle&&wanted==ticket){cancelled=true;sync::requestPoll();}}
void abandon(uint32_t wanted){if(state.load(std::memory_order_acquire)==State::Idle||wanted!=ticket)return;
    discard.store(true);cancelled.store(true);
    // A queued job may never gain admission under resource pressure. If the
    // worker has not claimed it, release it now so Retry can enqueue again.
    State queued=State::Queued;
    if(state.compare_exchange_strong(queued,State::Preparing)){cleanup();return;}
    State done=State::Complete;if(state.compare_exchange_strong(done,State::Preparing))cleanup();sync::requestPoll();}
} }
