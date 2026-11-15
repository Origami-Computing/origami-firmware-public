#include "services/app_http.h"
#include "core/memory.h"
/**
 * @file sync.cpp
 * @brief Install persistent Lua packages through the existing widget worker.
 *
 * Counterpart of the server's /api/device-widgets endpoints (kami2 branch
 * feature-device-widgets). v1 is deliberately simple: re-fetch every widget
 * on each sync rather than diffing updated_at. Unchanged packages are a no-op
 * in the persistent store. Bodies and work admission remain bounded.
 */

#include "widgets/sync.h"
#include "widgets/storage.h"
#include "services/custom_apps.h"
#include "core/health_policy.h"
#include "core/bounded_json_memory.h"
#include "config/backend_host.h"
#include <esp_heap_caps.h>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/tls_trust.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <atomic>
#include <mbedtls/sha256.h>

#include "apps/agent/agent_flow.h"
#include "services/eyes_asset.h"
#include "services/muni_transit.h"
#include "services/agent_coordination.h"
#include "services/background_network.h"
#include "core/secrets.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"

namespace widgets {
namespace sync {

namespace {

constexpr char kManifestPath[] = "/api/device-widgets";
constexpr size_t kMaxScriptBytes = 32u * 1024u;  // matches lua_app load cap
constexpr size_t kMaxDataBytes   = 8u * 1024u;   // matches lua_app data cap

// How often the poll task wakes up to check for new/updated widgets.
constexpr uint32_t kPollIntervalMs = 30000u;

// Accessed only by widget_poll, including its cancellation checkpoints.
static services::background_network::PeriodicWork s_periodicWork;
static std::atomic<TaskHandle_t> s_pollTask{nullptr};
static std::atomic<bool> s_pollStarting{false};
static std::atomic<bool> s_pauseRequested{false};
static std::atomic<bool> s_pauseAcknowledged{false};

bool pollPausePending() {
    return (s_pauseRequested || widgets::storage::cancelRequested()) && xTaskGetCurrentTaskHandle() == s_pollTask;
}

// Called only by widget_poll after it has released all HTTP/TLS and SD state.
// Returning true tells the caller to abandon the current poll cycle after the
// pause is released, rather than resuming a half-completed synchronization.
bool pauseAtIdleCheckpoint() {
    if (!s_pauseRequested || xTaskGetCurrentTaskHandle() != s_pollTask) return false;
    s_periodicWork.release();
    s_pauseAcknowledged = true;
    while (s_pauseRequested) {
        // Re-acknowledge if resume and the next PTT arrive before this task
        // wakes: resume clears the old acknowledgement to avoid stale state.
        s_pauseAcknowledged = true;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    s_pauseAcknowledged = false;
    return true;
}

void setErr(char* errOut, size_t errLen, const char* msg) {
    if (errOut && errLen > 0) snprintf(errOut, errLen, "%s", msg);
}

// A bounded sink also handles chunked HTTP bodies through HTTPClient's decoder.
// Reject oversize responses while streaming, before allocating an unbounded String.
class Body : public Stream {
public:
    explicit Body(size_t limit,services::custom_apps::Cancel cancel=nullptr,bool progress=false):progress_(progress),cancel_(cancel),started_(millis()),limit_(limit),bytes_(static_cast<char*>(heap_caps_malloc(limit+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))){}
    ~Body(){heap_caps_free(bytes_);}
    size_t write(uint8_t c) override {return write(&c,1);}
    size_t write(const uint8_t* p,size_t n) override {
        if((cancel_&&cancel_())||uint32_t(millis()-started_)>30000u||!bytes_)return 0;
        if(n>limit_-size_){overflow_=true;return 0;}memcpy(bytes_+size_,p,n);size_+=n;bytes_[size_]=0;if(progress_)widgets::storage::reportDownloadProgress(size_,total_);return n;
    }
    int available() override{return 0;}int read() override{return -1;}int peek() override{return -1;}void flush() override{}
    bool receive(HTTPClient& http){const int expected=http.getSize();total_=expected>0?size_t(expected):0;if(progress_)widgets::storage::reportDownloadProgress(0,total_);if(expected>int(limit_)){overflow_=true;return false;}if(!bytes_)return false;const int received=http.writeToStream(this);return received>0&&size_t(received)==size_&&(expected<0||size_t(expected)==size_)&&!(cancel_&&cancel_());}
    void reset(){total_=0;if(progress_)widgets::storage::reportDownloadProgress(0,0);size_=0;started_=millis();overflow_=false;if(bytes_)bytes_[0]=0;}
    bool ready() const{return bytes_!=nullptr;}
    bool overflowed() const{return overflow_;}
    const char* data() const{return bytes_;}size_t size() const{return size_;}
private:bool progress_;size_t total_=0;services::custom_apps::Cancel cancel_;uint32_t started_;size_t limit_,size_=0;char* bytes_;bool overflow_=false;
};
// Certificate verification is CPU-bound (seconds for a P-384 chain such as
// staging's) and this worker shares core 0 with net_poll and the idle task.
// Block briefly between requests so neither misses its task-watchdog deadline.
static void yieldBetweenRequests() { vTaskDelay(pdMS_TO_TICKS(50)); }

// Both code resolution and package retrieval use the same bounded public GET.
// Retry one transient transport/server failure, before any persistent publication.
// Release TLS objects before yielding; never retry malformed or oversized data.
static services::custom_apps::Result publicGet(const char* url,Body& body,
        services::custom_apps::Cancel cancel){
    namespace apps=services::custom_apps;
    if(!body.ready())return apps::Result::NoMemory;
    for(unsigned attempt=0;attempt<2;++attempt){
        if(cancel&&cancel())return apps::Result::Cancelled;
        if(WiFi.status()!=WL_CONNECTED)return apps::Result::Network;
        body.reset();int code=0;bool ok=false;
        Serial.printf("[app-store] GET attempt=%u internal=%u largest=%u\n",attempt+1,
            unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
        {
            WiFiClientSecure secure;services::tls::useRootBundle(secure);secure.setHandshakeTimeout(8);
            HTTPClient http;http.setConnectTimeout(4000);http.setTimeout(5000);
            http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
            if(!http.begin(secure,url))return apps::Result::Network;
            http.addHeader("X-Kami-App-Protocol","2");
#ifdef ORIGAMI_NATIVE_APPS
            http.addHeader("X-Kami-Native-HTTP","1");
            http.addHeader("X-Kami-Local-Play","1");
            http.addHeader("X-Kami-Catalog-Services","1");
#endif
            code=http.GET();ok=code==200&&body.receive(http);http.end();secure.stop();
        }
        if(cancel&&cancel())return apps::Result::Cancelled;
        if(ok)return apps::Result::Ok;
        if(code==426)return apps::Result::Incompatible;
        if(code==404 || code==410)return apps::Result::Missing;
        Serial.printf("[app-store] GET failed status=%d bytes=%u attempt=%u\n",code,unsigned(body.size()),attempt+1);
        const bool transient=code<0||code==408||code==429||code==500||code==502||code==503||code==504||code==200;
        if(body.overflowed())return apps::Result::Invalid;
        if(!transient||attempt==1)return apps::Result::Network;
        yieldBetweenRequests();
    }
    return apps::Result::Network;
}

static bool safeRemoteId(const char* id){if(!id||!id[0]||strlen(id)>79)return false;
    for(const char* p=id;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return false;
    return true;
}
// Worker-only. Caller owns TLS lifetime and periodic admission. Account-bound
// cloud namespace cannot overwrite local or legacy files, regardless of slug.
static bool fetchAndWriteWidget(const core::secrets::BackendConfig& config, WiFiClient& transport,
                                const char* id, const char* slug, services::custom_apps::Identity* installed=nullptr) {
    namespace apps=services::custom_apps;
    if(!safeRemoteId(id)||!slug||!slug[0]||strlen(slug)>48)return false;
    const auto request=core::secrets::backendRequest();
    if(strcmp(request.backend.url,config.url)||strcmp(request.backend.apiKey,config.apiKey))return false;
    apps::Identity identity;identity.medium=hal::sd::mounted()?apps::Medium::Sd:apps::Medium::Flash;
    char digest[65];
    if(!apps::accountOrigin(config.url,config.apiKey,identity.origin)||!apps::accountOrigin(id,"app-id",digest))return false;
    memcpy(identity.id,digest,31);identity.id[31]=0;
    const String base(config.url);
    HTTPClient get;get.setReuse(true);get.setConnectTimeout(4000);
    get.begin(transport,base+kManifestPath+"/"+id+"/main.lua");get.addHeader("X-Api-Key",config.apiKey);
    Body source(kMaxScriptBytes);const bool sourceOk=get.GET()==200 && source.receive(get);get.end();
    yieldBetweenRequests();if(!sourceOk||pollPausePending())return false;
    HTTPClient dataGet;dataGet.setReuse(true);dataGet.setConnectTimeout(4000);
    dataGet.begin(transport,base+kManifestPath+"/"+id+"/data.json");dataGet.addHeader("X-Api-Key",config.apiKey);
    Body data(kMaxDataBytes);const int code=dataGet.GET();
    const bool dataOk=code==404 || (code==200&&data.receive(dataGet));dataGet.end();
    yieldBetweenRequests();if(!dataOk||pollPausePending())return false;
    apps::Package package;
    auto result=apps::pack(identity.id,slug,"cloud",source.data(),source.size(),code==200?data.data():nullptr,code==200?data.size():0,package);
    if(result==apps::Result::Ok)result=widgets::storage::validateLua(package.bytes,package.size);
    // Revision and account checks guard stale response publication. Identity is
    // account-bound even if a setter races immediately after this comparison.
    if(core::secrets::backendRequest().revision!=request.revision)return false;
    if(result==apps::Result::Ok)result=apps::install(identity,package.bytes,package.size,pollPausePending);
    if(result!=apps::Result::Ok){Serial.printf("[widget-sync] install %s\n",apps::resultName(result));return false;}
    if(installed)*installed=identity;return true;
}

static void pollTask(void*) {
    s_pollTask = xTaskGetCurrentTaskHandle();
    if(widgets::storage::pending())xTaskNotifyGive(s_pollTask);
    bool retryAdmission = false;
    for (;;) {
        s_periodicWork.release();
        if (pauseAtIdleCheckpoint()) continue;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((retryAdmission || widgets::storage::pending() || services::muni::isActive() || services::app_http::hasOwner()) ? 500u : kPollIntervalMs));
        retryAdmission = false;
        if (pauseAtIdleCheckpoint()) continue;
        if (widgets::storage::pending()) {
            if (!core::health::backgroundPolicy().allowUploads) { retryAdmission=true; continue; }
            services::agent_coordination::TransportPause reconnectPause;
            // Foreground packages need DMA-capable TLS headroom even when the
            // Agent is connected. Hold its acknowledged transport pause through
            // the complete download; never interrupt an active Agent session.
            if ((widgets::storage::networkPending() || !services::background_network::PeriodicWork::hasMemoryReserve()) && WiFi.status() == WL_CONNECTED) {
                if (services::agent_coordination::isSessionActive() || !reconnectPause.acquire(1000)) {
                    retryAdmission=true;continue;
                }
                if (pollPausePending() || services::agent_coordination::isSessionActive()) continue;
            }
            if (!s_periodicWork.tryEnter(widgets::storage::networkPending())) {
                static uint32_t logged=0;
                if(uint32_t(millis()-logged)>10000){logged=millis();core::memory::logSnapshot("widget-storage-admission");}
                retryAdmission=true; continue;
            }
            widgets::storage::process(pollPausePending);
            reconnectPause.release();
            s_periodicWork.release();
            if (pauseAtIdleCheckpoint()) continue;
            // Jobs are bounded foreground work; revisit transit on next wake.
            retryAdmission=true;continue;
        }
        if (!core::health::backgroundPolicy().allowUploads || WiFi.status() != WL_CONNECTED) continue;
        const bool liveHttp = services::app_http::hasOwner();
        const bool liveTransit = services::muni::isActive();
        if ((liveTransit || liveHttp) && !services::muni::pollDue() && !services::app_http::pollDue()) continue;
        const bool agentReady = apps::agent::flow::isTransportReady();
        if (!agentReady && !liveTransit && !liveHttp) continue;
        services::agent_coordination::TransportPause reconnectPause;
        if (!agentReady || liveHttp) {
            if (services::agent_coordination::isSessionActive() ||
                !reconnectPause.acquire(1000)) { retryAdmission = true; continue; }
            if (pollPausePending() || services::agent_coordination::isSessionActive()) continue;
        }
        if (!s_periodicWork.tryEnter()) { retryAdmission = true; continue; }
        services::app_http::poll(pollPausePending);
        if (!pollPausePending()) services::muni::poll(pollPausePending);
        reconnectPause.release();
        if (pauseAtIdleCheckpoint()) continue;
        if (services::muni::isActive() || services::app_http::hasOwner()) continue;

        // Net polling is disabled in some hardware-diagnostic builds. Offer
        // the same one-shot recovery opportunity from this existing TLS-sized
        // worker once the primary agent transport has settled.
        services::eyes_asset::service();
        if (pauseAtIdleCheckpoint()) continue;

    }
}

}  // namespace

services::custom_apps::Result download(core::app_store::Download& request,
        services::custom_apps::Package& out,services::custom_apps::Cancel cancel) {
    namespace apps=services::custom_apps;
    if(request.code[0]) {
        core::app_store::Download code;
        if(strnlen(request.code,sizeof(request.code))!=6 ||
           !core::app_store::parse(request.code,code))return apps::Result::Invalid;
        if(cancel&&cancel())return apps::Result::Cancelled;
        if(WiFi.status()!=WL_CONNECTED)return apps::Result::Network;
        core::app_store::Download package;
        {
            char url[192];snprintf(url,sizeof(url),
                "%s/api/app-store/code/%s",code.code[0]=='0'?KAMI_TEST_INSTALL_STORE_URL:KAMI_APP_STORE_URL,code.code);
            Body body(255,cancel);
            const auto fetched=publicGet(url,body,cancel);
            if(fetched!=apps::Result::Ok)return fetched;
            char resolved[256]={};memcpy(resolved,body.data(),body.size());
            // Resolve once only; embedded NUL and another code are invalid responses.
            if(strlen(resolved)!=body.size()||!core::app_store::parse(resolved,package)||
               package.code[0])return apps::Result::Invalid;
        } // Release all resolver transport objects before the next TLS handshake.
        yieldBetweenRequests();
        auto result=download(package,out,cancel);
        if(result==apps::Result::Ok && code.code[0]!='0'){
            // Public catalog codes install signed native apps. Private test codes
            // and the separate Talk to App workflow retain their own contracts.
            apps::View view;
            result=apps::inspect(out.bytes,out.size,view);
            if(result==apps::Result::Ok && !view.native)result=apps::Result::Incompatible;
            if(result!=apps::Result::Ok)out.reset();
        }
        if(result==apps::Result::Ok)request=package; // Preserve URL-scoped install identity.
        return result;
    }
    if(strnlen(request.url,sizeof(request.url))==sizeof(request.url)||
       strnlen(request.sha256,sizeof(request.sha256))!=64)return apps::Result::Invalid;
    char qr[280];snprintf(qr,sizeof(qr),"origami-app:%s#sha256=%s",request.url,request.sha256);
    core::app_store::Download checked;if(!core::app_store::parse(qr,checked))return apps::Result::Invalid;
    if(cancel&&cancel())return apps::Result::Cancelled;
    if(WiFi.status()!=WL_CONNECTED)return apps::Result::Network;
    // No credentials or redirects; the QR's hash still pins the final bytes.
    Body body(apps::kPackageBytes,cancel,true);
    const auto fetched=publicGet(request.url,body,cancel);
    if(fetched!=apps::Result::Ok)return fetched;
    uint8_t hash[32];char hex[65];
    if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(body.data()),body.size(),hash,0))return apps::Result::Io;
    for(size_t i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);
    if(strcmp(hex,request.sha256))return apps::Result::Corrupt;
    apps::View view;auto result=apps::inspect(reinterpret_cast<const uint8_t*>(body.data()),body.size(),view);
    if(result!=apps::Result::Ok)return result;
    auto* bytes=static_cast<uint8_t*>(heap_caps_malloc(body.size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!bytes)return apps::Result::NoMemory;
    memcpy(bytes,body.data(),body.size());out.reset();out.bytes=bytes;out.size=body.size();return apps::Result::Ok;
}

int syncOne(const char* id, const char* slug, char* errOut, size_t errLen, services::custom_apps::Identity* installed) {
    if (!id || !slug || !slug[0]) { setErr(errOut, errLen, "bad id/slug"); return -1; }
    if (WiFi.status() != WL_CONNECTED) { setErr(errOut, errLen, "wifi not connected"); return -1; }
    const auto config = core::secrets::backendConfig();
    WiFiClient plain;
    WiFiClientSecure secure;
    services::tls::useRootBundle(secure);
    secure.setHandshakeTimeout(8);
    WiFiClient& transport = strncmp(config.url, "https://", 8) == 0
                                ? static_cast<WiFiClient&>(secure) : plain;
    if (!fetchAndWriteWidget(config, transport, id, slug, installed)) { setErr(errOut, errLen, "fetch/write failed"); return -1; }
    return 0;
}

bool startPolling() {
    bool idle=false;
    if (s_pollTask || !s_pollStarting.compare_exchange_strong(idle,true)) return true;
    if (!hal::tasks::spawn(hal::tasks::Id::WidgetPoll, pollTask)) { s_pollStarting=false; return false; }
    return true;
}

void requestPoll() {
    TaskHandle_t task = s_pollTask;
    if (task) xTaskNotifyGive(task);
}

void requestPollingPause() {
    if (s_pauseRequested) return;
    s_pauseRequested = true;

    TaskHandle_t task = s_pollTask;
    if (task) xTaskNotifyGive(task);
}

bool pollingPaused() { return !s_pollTask || (s_pauseRequested && s_pauseAcknowledged); }

bool suspendPolling(uint32_t timeoutMs) {
    requestPollingPause();
    TaskHandle_t task = s_pollTask;
    if (!task) return true;
    xTaskNotifyGive(task);

    const uint32_t startedMs = millis();
    while (!s_pauseAcknowledged && (uint32_t)(millis() - startedMs) < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_pauseAcknowledged) {
        Serial.printf("[widget-poll] paused in %lu ms\n",
                      (unsigned long)(millis() - startedMs));
        return true;
    }

    s_pauseRequested = false;
    xTaskNotifyGive(task);
    Serial.printf("[widget-poll] pause timed out after %lu ms\n",
                  (unsigned long)(millis() - startedMs));
    return false;
}

void resumePolling() {
    if (!s_pauseRequested) return;
    s_pauseRequested = false;
    TaskHandle_t task = s_pollTask;
    if (task) xTaskNotifyGive(task);
    Serial.println("[widget-poll] resumed");
}

}  // namespace sync
}  // namespace widgets
