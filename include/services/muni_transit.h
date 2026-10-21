#pragma once
#include "core/muni_feed.h"
namespace services { namespace muni {
struct Snapshot {
    core::muni::Feed feed;
    uint32_t generation=0, updatedAt=0;
    bool active=false, error=false;
    char stop[6]={};
};
// One current widget subscription. UI task only; replaces its previous request.
// Fixed SFMTA origin/route; numeric stop code only. No network work here.
uint32_t watch(const char* stop);
void stop(uint32_t generation); // Stale owner's release never cancels a new watch.
Snapshot snapshot(); // Coherent owned copy, no network or allocations.
// Existing WidgetPoll task only, under its normal network admission.
// Cancellation is cooperative; worker pause acknowledges only after HTTP cleanup.
bool isActive(); // Cheap synchronized lifecycle query without copying cached feed.
bool pollDue(); // Worker only: 30-second refresh, 5-second minimum for a changed stop.
void poll(bool (*cancelled)());
} }
