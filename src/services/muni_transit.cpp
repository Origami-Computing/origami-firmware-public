#include "services/muni_transit.h"
#include "services/tls_trust.h"
#include "core/board_time.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <time.h>
namespace services { namespace muni {
namespace {
portMUX_TYPE mutex=portMUX_INITIALIZER_UNLOCKED;
Snapshot state;
uint32_t lastAttempt=0;
bool attempted=false;
char lastStop[6]={};
constexpr size_t kBodyLimit=131072;
class Body : public Stream {
public:
    Body(bool (*cancel)()):cancel_(cancel),started_(millis()) {data=(char*)heap_caps_malloc(kBodyLimit+1,MALLOC_CAP_SPIRAM);}
    ~Body(){heap_caps_free(data);}
    size_t write(uint8_t c) override {return write(&c,1);}
    size_t write(const uint8_t* bytes,size_t n) override {
        if(!data||n>kBodyLimit-size||cancel_()||uint32_t(millis()-started_)>12000) return 0;
        memcpy(data+size,bytes,n);size+=n;data[size]=0;return n;
    }
    int available() override{return 0;}int read() override{return -1;}int peek() override{return -1;}void flush() override{}
    char* data=nullptr;size_t size=0;
private:bool (*cancel_)();uint32_t started_;
};
bool fetch(const char* path, bool (*cancelled)(),core::muni::Feed& feed,const char* stopCode=nullptr) {
    if(cancelled())return false;
    Body body(cancelled);if(!body.data)return false;
    WiFiClientSecure client;tls::useRootBundle(client);client.setHandshakeTimeout(8);
    HTTPClient http;http.useHTTP10(true);http.setConnectTimeout(4000);http.setTimeout(2000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    char url[160];snprintf(url,sizeof(url),"https://sfmta.gtfs.media/gtfs/api/v2/rt/%s",path);
    if(!http.begin(client,url))return false;
    http.setUserAgent("KamiTransit/0.1");http.addHeader("Accept","application/json");
    const int code=http.GET();
    bool ok=code==200 && http.getSize()<=int(kBodyLimit) && !cancelled();
    if(ok)ok=http.writeToStream(&body)>=0 && body.size>0;
    http.end();
    if(!ok||cancelled())return false;
    return stopCode ? core::muni::parseArrivals(body.data,body.size,stopCode,uint32_t(time(nullptr)),feed)
                    : core::muni::parseVehicles(body.data,body.size,feed);
}
}
uint32_t watch(const char* stopCode) {
    if(!stopCode||strlen(stopCode)!=5)return 0;
    for(unsigned i=0;i<5;++i)if(stopCode[i]<'0'||stopCode[i]>'9')return 0;
    portENTER_CRITICAL(&mutex);
    uint32_t next=state.generation+1;if(!next)next=1;
    if(strcmp(state.stop,stopCode)!=0)state=Snapshot{};
    state.generation=next;state.active=true;memcpy(state.stop,stopCode,6);
    portEXIT_CRITICAL(&mutex);
    Serial.printf("[muni] watch stop=%s generation=%lu at_ms=%lu\n",stopCode,(unsigned long)next,(unsigned long)millis());
    return next;
}
void stop(uint32_t generation) {
    portENTER_CRITICAL(&mutex);if(state.generation==generation)state.active=false;portEXIT_CRITICAL(&mutex);
}
Snapshot snapshot(){portENTER_CRITICAL(&mutex);auto copy=state;portEXIT_CRITICAL(&mutex);return copy;}
bool isActive() {portENTER_CRITICAL(&mutex);bool active=state.active;portEXIT_CRITICAL(&mutex);return active;}
bool pollDue() {
    char stopCode[6];bool active,hasArrivals;
    portENTER_CRITICAL(&mutex);active=state.active;hasArrivals=state.feed.predictionTimestamp!=0;memcpy(stopCode,state.stop,6);portEXIT_CRITICAL(&mutex);
    const uint32_t interval=hasArrivals && strcmp(lastStop,stopCode)==0?30000:5000;
    return active && core::time::isSynced() &&
           (!attempted || uint32_t(millis()-lastAttempt)>=interval);
}
void poll(bool (*cancelled)()) {
    auto request=snapshot();
    if(!request.active||cancelled()||!pollDue())return;
    const uint32_t now=millis();

    attempted=true;lastAttempt=now;memcpy(lastStop,request.stop,6);
    char path[40];snprintf(path,sizeof(path),"tripupdates/%s",request.stop);
    Serial.printf("[muni] request stop=%s at_ms=%lu\n",request.stop,(unsigned long)now);
    bool arrivals=fetch(path,cancelled,request.feed,request.stop);
    portENTER_CRITICAL(&mutex);
    const bool current=state.active&&state.generation==request.generation;
    portEXIT_CRITICAL(&mutex);
    if(!current||cancelled())return; // A dial change makes the old map fetch unnecessary.
    // Arrivals are independently useful. Do not hold them behind the map request.
    if(arrivals && !cancelled()) {
        request.error=false;request.updatedAt=uint32_t(time(nullptr));
        portENTER_CRITICAL(&mutex);
        if(state.active&&state.generation==request.generation)state=request;
        portEXIT_CRITICAL(&mutex);
        Serial.printf("[muni] arrivals stop=%s count=%u duration_ms=%lu\n",request.stop,request.feed.arrivalCount,(unsigned long)(millis()-now));
    }
    bool buses=!cancelled()&&fetch("vehicles/5",cancelled,request.feed);
    if(cancelled())return;
    request.error=!arrivals||!buses;request.updatedAt=uint32_t(time(nullptr));
    portENTER_CRITICAL(&mutex);
    if(state.active&&state.generation==request.generation)state=request;
    portEXIT_CRITICAL(&mutex);
    Serial.printf("[muni] stop=%s arrivals=%u buses=%u ok=%u duration_ms=%lu\n",request.stop,request.feed.arrivalCount,request.feed.busCount,!request.error,(unsigned long)(millis()-now));
}
} }
