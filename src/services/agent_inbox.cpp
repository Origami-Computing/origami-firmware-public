#include "services/agent_inbox.h"
#include "services/agent_delivery.h"
#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "core/memory.h"
#include "widgets/launch.h"

namespace services { namespace agent_inbox {
namespace {
constexpr size_t kMaxReplies = 4;
constexpr size_t kMaxReplyBytes = 16u * 1024u;
constexpr size_t kMaxWidgetActions = 4;

enum class WidgetAction : uint8_t { Loading, Ready, Error };
struct WidgetFrame { WidgetAction action; char first[64]; char second[64]; };

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char* s_replies[kMaxReplies] = {};
char s_receipts[kMaxReplies][48] = {};
bool s_accepting = false;
size_t s_replyHead = 0, s_replyCount = 0;
uint64_t s_lastEventId = 0;
WidgetFrame s_widgets[kMaxWidgetActions] = {};
size_t s_widgetHead = 0, s_widgetCount = 0;
}

bool offerReply(uint64_t eventId, const char* text, const char* receiptId) {
    if (!text || !text[0]) return false;
    portENTER_CRITICAL(&s_mux);
    const bool accepting = s_accepting;
    const bool seen = eventId != 0 && eventId <= s_lastEventId;
    if(!accepting && eventId>s_lastEventId)s_lastEventId=eventId;
    portEXIT_CRITICAL(&s_mux);
    if (!accepting) { agent_delivery::reject(receiptId,"left_app"); return false; }
    if (seen) return false;

    const size_t length = strnlen(text, kMaxReplyBytes - 1);
    void* storage = core::memory::allocPsram("agent.inbox", length + 1);
    if (!storage) {
        agent_delivery::reject(receiptId,"buffer_full");
        Serial.printf("[agent-inbox] reply %llu dropped: no PSRAM\n", (unsigned long long)eventId);
        return false;
    }
    char* copy = static_cast<char*>(storage);
    memcpy(copy, text, length);
    copy[length] = '\0';

    char* evicted = nullptr;
    char evictedReceipt[48] = {};
    portENTER_CRITICAL(&s_mux);
    if (!s_accepting) {
        if(eventId>s_lastEventId)s_lastEventId=eventId;
        portEXIT_CRITICAL(&s_mux);
        agent_delivery::reject(receiptId,"left_app");
        core::memory::freePsram(storage);return false;
    }
    if (eventId > s_lastEventId) s_lastEventId = eventId;
    if (s_replyCount == kMaxReplies) {
        evicted = s_replies[s_replyHead];
        strlcpy(evictedReceipt,s_receipts[s_replyHead],sizeof(evictedReceipt));
        s_replies[s_replyHead] = nullptr;
        s_replyHead = (s_replyHead + 1) % kMaxReplies;
        s_replyCount--;
    }
    strlcpy(s_receipts[(s_replyHead+s_replyCount)%kMaxReplies],receiptId,sizeof(s_receipts[0]));
    s_replies[(s_replyHead + s_replyCount) % kMaxReplies] = copy;
    s_replyCount++;
    portEXIT_CRITICAL(&s_mux);
    if (evicted) {
        agent_delivery::reject(evictedReceipt,"buffer_full");
        void* old = evicted;
        core::memory::freePsram(old);
        Serial.println("[agent-inbox] inbox full; dropped the oldest reply");
    }
    Serial.printf("[agent-inbox] reply id=%llu chars=%u\n", (unsigned long long)eventId, (unsigned)length);
    return true;
}

bool offerWidget(const char* action, const char* first, const char* second) {
    WidgetFrame frame{};
    if (!action) return false;
    if (!strcmp(action, "widget_loading")) frame.action = WidgetAction::Loading;
    else if (!strcmp(action, "widget_ready")) frame.action = WidgetAction::Ready;
    else if (!strcmp(action, "widget_error")) frame.action = WidgetAction::Error;
    else return false;
    strlcpy(frame.first, first ? first : "", sizeof(frame.first));
    strlcpy(frame.second, second ? second : "", sizeof(frame.second));
    if (frame.action == WidgetAction::Ready && (!frame.first[0] || !frame.second[0])) return false;
    portENTER_CRITICAL(&s_mux);
    if (s_widgetCount == kMaxWidgetActions) {
        s_widgetHead = (s_widgetHead + 1) % kMaxWidgetActions;
        s_widgetCount--;
    }
    s_widgets[(s_widgetHead + s_widgetCount) % kMaxWidgetActions] = frame;
    s_widgetCount++;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

uint64_t lastReplyEventId() {
    portENTER_CRITICAL(&s_mux);
    const uint64_t id = s_lastEventId;
    portEXIT_CRITICAL(&s_mux);
    return id;
}

void poll() {
    for (;;) {
        WidgetFrame frame;
        portENTER_CRITICAL(&s_mux);
        const bool available = s_widgetCount > 0;
        if (available) {
            frame = s_widgets[s_widgetHead];
            s_widgetHead = (s_widgetHead + 1) % kMaxWidgetActions;
            s_widgetCount--;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!available) return;
        switch (frame.action) {
            case WidgetAction::Loading: widgets::launch::notifyLoading(frame.first[0] ? frame.first : "widget"); break;
            case WidgetAction::Ready:   widgets::launch::notifyReady(frame.first, frame.second); break;
            case WidgetAction::Error:   widgets::launch::notifyError(frame.first[0] ? frame.first : "generation failed"); break;
        }
    }
}

bool hasReply() {
    portENTER_CRITICAL(&s_mux);
    const bool pending = s_replyCount > 0;
    portEXIT_CRITICAL(&s_mux);
    return pending;
}

bool takeReply(bool (*show)(const char* text, const char* receiptId)) {
    // Only ui_task consumes, so the head entry cannot be taken concurrently;
    // the producer may only evict it when the inbox is full.
    portENTER_CRITICAL(&s_mux);
    char receipt[48] = {};
    if(s_replyCount) strlcpy(receipt,s_receipts[s_replyHead],sizeof(receipt));
    char* text = s_replyCount ? s_replies[s_replyHead] : nullptr;
    if (text) {
        s_replies[s_replyHead] = nullptr;
        s_replyHead = (s_replyHead + 1) % kMaxReplies;
        s_replyCount--;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!text) return false;
    const bool shown = show && show(text,receipt);
    if (!shown) {
        // Put it back at the head so it is offered again on the next pass.
        portENTER_CRITICAL(&s_mux);
        if (s_replyCount < kMaxReplies) {
            s_replyHead = (s_replyHead + kMaxReplies - 1) % kMaxReplies;
            s_replies[s_replyHead] = text;
            strlcpy(s_receipts[s_replyHead],receipt,sizeof(s_receipts[0]));
            s_replyCount++;
            text = nullptr;
        }
        portEXIT_CRITICAL(&s_mux);
    }
    if (text) { void* p = text; core::memory::freePsram(p); }
    return shown;
}
void setAccepting(bool accepting) {
    char* removed[kMaxReplies]{}; char receipts[kMaxReplies][48]{};
    portENTER_CRITICAL(&s_mux);
    s_accepting=accepting;
    if(!accepting){
        for(size_t i=0;i<kMaxReplies;++i){removed[i]=s_replies[i];s_replies[i]=nullptr;strlcpy(receipts[i],s_receipts[i],sizeof(receipts[i]));}
        s_replyCount=s_replyHead=0;
    }
    portEXIT_CRITICAL(&s_mux);
    for(size_t i=0;i<kMaxReplies;++i)if(removed[i]){agent_delivery::reject(receipts[i],"left_app");void* p=removed[i];core::memory::freePsram(p);}
}
} }
