#pragma once
#include <stddef.h>
namespace services { namespace agent_delivery {
// Worker records receipt of a complete frame; UI binds it to its reply segment.
// All methods are bounded/thread-safe. Socket I/O stays on the transport worker.
void received(const char* id);
void bind(const char* id, int segment);
void displayed(int segment); // UI only, after display transfer and visible text.
void reject(const char* id, const char* reason);
void replaceView(); // UI: retire the previous reply without rejecting queued new replies.
void dismiss(); // UI: reject every undisplayed reply when leaving the workflow.
// Worker: receipts are scoped to the authenticated socket; backend logs unresolved ones on close.
void disconnected();
void confirmed(const char* id, const char* stage);
void pump(bool (*send)(const char*)); // Worker, retries until server confirms.
} }
