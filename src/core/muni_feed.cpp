#include "core/muni_feed.h"
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif
namespace core { namespace muni {
namespace {
// Bound parser allocation independently of the transport's raw body budget.
class JsonMemory : public ArduinoJson::Allocator {
    size_t used=0;
    struct alignas(max_align_t) Header {size_t size;};
public:
    void* allocate(size_t n) override {
        if(n>196608-used)return nullptr;
#ifdef ARDUINO
        auto* h=(Header*)heap_caps_malloc(n+sizeof(Header),MALLOC_CAP_SPIRAM);
#else
        auto* h=(Header*)malloc(n+sizeof(Header));
#endif
        if(!h)return nullptr;h->size=n;used+=n;return h+1;
    }
    void deallocate(void* p) override {if(p){auto* h=(Header*)p-1;used-=h->size;free(h);}}
    void* reallocate(void* p,size_t n) override {
        if(!p)return allocate(n);
        auto* h=(Header*)p-1;void* next=allocate(n);if(!next)return nullptr;
        memcpy(next,p,n<h->size?n:h->size);deallocate(p);return next;
    }
};
// Raw body is capped by transport. Filter retains just the fields we use.
uint32_t timestamp(JsonVariantConst v) {
    if (v.is<uint32_t>()) return v.as<uint32_t>();
    const char* s=v.as<const char*>();
    if (!s || !*s || strlen(s)>10) return 0;
    uint64_t n=0; for (;*s;++s) {if (*s<'0'||*s>'9') return 0; n=n*10+(*s-'0');}
    return n<=UINT32_MAX ? uint32_t(n) : 0;
}
bool route5(JsonObjectConst row) {return strcmp(row["trip"]["routeId"]|"", "5")==0;}
}
bool parseVehicles(const char* json, size_t length, Feed& out) {
    if (length>131072) return false;
    JsonMemory memory;JsonDocument filter(&memory), doc(&memory);
    filter["meta"]["last_fetched"]=true;
    JsonObject f=filter["data"][0].to<JsonObject>(); f["trip"]["routeId"]=true;f["position"]=true;f["timestamp"]=true;
    if (deserializeJson(doc,json,length,DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(10)) || !doc["data"].is<JsonArray>()) return false;
    Feed result;result.vehicleTimestamp=timestamp(doc["meta"]["last_fetched"]);
    for (JsonObjectConst row:doc["data"].as<JsonArrayConst>()) {
        if (!route5(row) || result.busCount==kMaxBuses) continue;
        auto p=row["position"];
        if (!p["latitude"].is<double>() || !p["longitude"].is<double>()) continue;
        const float lat=p["latitude"],lon=p["longitude"];
        if (!std::isfinite(lat)||!std::isfinite(lon)||lat<37.6||lat>38||lon<-122.7||lon>-122.2) continue;
        auto& b=result.buses[result.busCount++];b.latitude=lat;b.longitude=lon;b.timestamp=timestamp(row["timestamp"]);
    }
    out.busCount=result.busCount;out.vehicleTimestamp=result.vehicleTimestamp;
    memcpy(out.buses,result.buses,sizeof(out.buses)); return true;
}
bool parseArrivals(const char* json, size_t length, const char* stop, uint32_t now, Feed& out) {
    if (length>131072) return false;
    JsonMemory memory;JsonDocument filter(&memory),doc(&memory);
    filter["meta"]["feed_timestamp"]=true;
    JsonObject f=filter["data"][0].to<JsonObject>();f["trip"]=true;f["stopTimeUpdate"]=true;
    if (deserializeJson(doc,json,length,DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(10)) || !doc["data"].is<JsonArray>()) return false;
    Feed result;result.predictionTimestamp=timestamp(doc["meta"]["feed_timestamp"]);
    for (JsonObjectConst row:doc["data"].as<JsonArrayConst>()) {
        if (!route5(row)||strcmp(row["trip"]["scheduleRelationship"]|"","CANCELED")==0) continue;
        const char* trip=row["trip"]["tripId"]|"";
        if (!*trip||strlen(trip)>=sizeof(Arrival::trip)) continue;
        auto updates=row["stopTimeUpdate"].as<JsonArrayConst>();
        unsigned index=0;
        for (JsonObjectConst u:updates) {
            ++index;
            if (strcmp(u["stopId"]|"",stop)!=0 || strcmp(u["scheduleRelationship"]|"","SKIPPED")==0) continue;
            if (index==updates.size() && u["departure"].isNull()) continue;
            uint32_t t=timestamp(u["departure"]["time"]);if (!t)t=timestamp(u["arrival"]["time"]);
            if (!t||int64_t(t)<int64_t(now)-30||int64_t(t)>int64_t(now)+7200) continue;
            bool duplicate=false;for(unsigned i=0;i<result.arrivalCount;++i)if(strcmp(result.arrivals[i].trip,trip)==0)duplicate=true;
            if (duplicate) continue;
            unsigned pos=0;while(pos<result.arrivalCount&&result.arrivals[pos].epoch<=t)++pos;
            if(pos==kMaxArrivals)continue;
            if(result.arrivalCount<kMaxArrivals)++result.arrivalCount;
            for(unsigned j=result.arrivalCount-1;j>pos;--j)result.arrivals[j]=result.arrivals[j-1];
            auto& a=result.arrivals[pos];a.epoch=t;
            snprintf(a.trip,sizeof(a.trip),"%s",trip);
            snprintf(a.headsign,sizeof(a.headsign),"%s",row["trip"]["tripHeadsign"]|"");
        }
    }
    out.arrivalCount=result.arrivalCount;out.predictionTimestamp=result.predictionTimestamp;
    memcpy(out.arrivals,result.arrivals,sizeof(out.arrivals));return true;
}
} }
