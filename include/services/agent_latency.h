#pragma once
namespace services { namespace agent_latency {
// UI-task owned. Measures submission to a completed display transfer, not tokens
// produced by the server. Preview/inbox/revisited replies never start a trace.
void begin();
void submit();
// UI task only. Static short event name (<=12 chars), never user text.
void milestone(const char* name);
void replyReady();
void painted(int top, int bottom);
void cancel();
void fail();
} }
