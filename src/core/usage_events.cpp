#include "core/usage_events.h"
#include "core/telemetry_queue.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <atomic>
#include <stdio.h>
#include <time.h>
#include <string.h>
namespace core { namespace usage {
namespace { char boot[33] = {}; std::atomic<uint32_t> sequence{0};
bool identifier(const char* s, size_t cap) {
 if (!s || !*s || strlen(s)>cap) return false;
 for (;*s;++s) if (!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='.'||*s=='-')) return false;
 return true;
}}
void init() { snprintf(boot,sizeof(boot),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random()); }
void emit(const char* app,const char* event,int64_t durationMs) {
 if (!boot[0] || !identifier(app,48) || !identifier(event,24)) return;
 char json[telemetry_queue::kItemJsonCap];
 const auto seq=sequence.fetch_add(1);
 const auto up=esp_timer_get_time()/1000;
 const time_t epoch=time(nullptr);
 const int n=snprintf(json,sizeof(json),"{\"v\":1,\"boot\":\"%s\",\"seq\":%lu,\"uptime_ms\":%lld,\"ts\":%lld,\"app\":\"%s\",\"event\":\"%s\"",
  boot,(unsigned long)seq,(long long)up,(long long)(epoch>=1704067200?epoch:0),app,event);
 if(n<0 || n>=(int)sizeof(json)) return;
 const int end=durationMs>=0 ? snprintf(json+n,sizeof(json)-n,",\"duration_ms\":%lld}",(long long)durationMs) : snprintf(json+n,sizeof(json)-n,"}");
 if(end<0 || end>=(int)sizeof(json)-n) return;
 telemetry_queue::enqueue(telemetry_queue::KIND_USAGE,json);
}
} }
