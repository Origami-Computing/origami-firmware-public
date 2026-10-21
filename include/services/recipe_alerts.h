#pragma once
#include "services/automation.h"
#include <string.h>

namespace services { namespace recipe_alerts {
constexpr unsigned kCapacity = 12;
constexpr unsigned kReceiptCount = 32;
constexpr time_t kMaxLateness = 60;
struct Entry {
  char id[65];
  automation::Type type;
  time_t fire;
  char message[128];
};
struct Snapshot { unsigned count = 0; Entry items[kCapacity]{}; };
struct Receipts { unsigned cursor = 0; char ids[kReceiptCount][65]{}; };

// UI-thread owned. Remote reminders have their own capacity, never displacing
// local alarms or timers. A complete validated snapshot replaces this queue.
class Queue {
 public:
  Snapshot pending;
  Receipts receipts;
  bool handled(const char* id) const {
    for (const auto& known : receipts.ids) if (!strcmp(known, id)) return true;
    return false;
  }
  void clear() { pending.count = 0; }
  void replace(const Snapshot& snapshot) {
    pending.count = 0;
    for (unsigned i=0; i<snapshot.count; ++i)
      if (!handled(snapshot.items[i].id)) pending.items[pending.count++] = snapshot.items[i];
  }
  bool upsert(const Entry& entry) {
    if (handled(entry.id)) return true;
    for (unsigned i=0; i<pending.count; ++i) if (!strcmp(pending.items[i].id,entry.id)) {
      pending.items[i]=entry; return true;
    }
    if (pending.count==kCapacity) return false;
    pending.items[pending.count++]=entry; return true;
  }
  bool takeDue(time_t now, Entry& entry) {
    // Discard overdue reminders, but do not alter local alarm semantics.
    for (unsigned i=0; i<pending.count;) {
      if (pending.items[i].fire < now-kMaxLateness) pending.items[i]=pending.items[--pending.count];
      else ++i;
    }
    int best=-1;
    for (unsigned i=0;i<pending.count;++i)
      if (pending.items[i].fire<=now && (best<0 || pending.items[i].fire<pending.items[best].fire)) best=i;
    if(best<0) return false;
    entry=pending.items[best]; pending.items[best]=pending.items[--pending.count]; return true;
  }
  void remember(const char* id) {
    auto& slot=receipts.ids[receipts.cursor++ % kReceiptCount];
    strncpy(slot,id,sizeof(slot)); slot[sizeof(slot)-1]=0;
    receipts.cursor %= kReceiptCount;
  }
  bool earliest(time_t& when) const {
    if (!pending.count) return false;
    when=pending.items[0].fire;
    for(unsigned i=1;i<pending.count;++i) if(pending.items[i].fire<when) when=pending.items[i].fire;
    return true;
  }
};
} }
