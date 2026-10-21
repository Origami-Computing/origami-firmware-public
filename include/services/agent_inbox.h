#pragma once
#include <stddef.h>
#include <stdint.h>

namespace services { namespace agent_inbox {
// Server-initiated agent frames that arrive outside a voice turn: late replies
// (delegation results, push_v1) and widget generation progress (widget_v1).
//
// Producer: the agent transport worker (Core 0). Consumer: ui_task. Text is
// copied into PSRAM before publication; no allocation or callback runs under
// the lock. Bounded: the oldest reply is dropped when the inbox is full.

// Worker: queue a reply. Ignores ids at or below the last one accepted, so a
// resumed stream that replays an event cannot show it twice.
bool offerReply(uint64_t eventId, const char* text, const char* receiptId = "");
// Worker: queue a widget_loading / widget_ready / widget_error action.
bool offerWidget(const char* action, const char* first, const char* second);
// Worker: resume cursor for the next `hello` after a reconnect (0 = none).
uint64_t lastReplyEventId();

// ui_task, every main-loop pass: apply queued widget actions to widgets::launch.
void poll();
// ui_task: true while a reply waits for the Agent to show it.
bool hasReply();
// ui_task: hand the oldest reply to `show`; it is removed only when `show`
// returns true. The pointer is valid only during the call.
bool takeReply(bool (*show)(const char* text, const char* receiptId));
// UI: close reception on app departure; pending replies are rejected and freed.
void setAccepting(bool accepting);
} }
