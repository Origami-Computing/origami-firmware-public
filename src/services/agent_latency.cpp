#include "services/agent_latency.h"
#include "core/telemetry.h"
#include "hal/hal_display.h"
#include <Arduino.h>
#include <esp_random.h>

namespace services { namespace agent_latency {
namespace {
uint32_t boot=0, sequence=0, started=0, submitted=0;
bool active=false, sent=false, ready=false, armed=false;
int firstRow=0,lastRow=0;
void event(const char* name,uint32_t now) {
    // <=95 characters even at uint32 maxima: survives existing fleet log cap.
    // Monotonic deltas avoid device/server clock skew. Never include user text.
    LOGI("reply-latency","v=2 t=%08lx%08lx e=%s ms=%lu up=%lu",
        (unsigned long)boot,(unsigned long)sequence,name,
        (unsigned long)(now-(sent?submitted:started)),(unsigned long)now);
}
void stop(const char* outcome) {
    if(active)event(outcome,millis());
    active=false;armed=false;hal::display::setFlushObserver(nullptr);
}
void flushed(uint32_t at,int top,int bottom) {
    if(!active||!sent||!armed||top>firstRow||bottom<lastRow)return;
    event("display",at);active=false;armed=false;
    hal::display::setFlushObserver(nullptr);
}
}
void begin() {
    stop("cancel");
    if(!boot)boot=esp_random();
    ++sequence;started=millis();sent=false;ready=false;active=true;
    event("capture",started);
}
void submit() {
    if(!active||sent)return;
    submitted=millis();sent=true;event("submit",submitted);
}
void replyReady() {
    if(!active||!sent||ready)return;
    ready=true;event("ready",millis());
}
void painted(int top,int bottom) {
    if(!active||!sent||!ready||armed)return;
    event("paint",millis());
    firstRow=top;lastRow=bottom;armed=true;
    hal::display::setFlushObserver(flushed);
}
void milestone(const char* name) { if(active)event(name,millis()); }
void cancel(){stop("cancel");}
void fail(){stop("error");}
} }
