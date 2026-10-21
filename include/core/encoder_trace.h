#pragma once
#include <stdint.h>
#include <stddef.h>

namespace core { namespace rotary {
// Fixed recording, never overwrites evidence. Owner supplies synchronization.
enum class TraceKind : uint8_t { Start, Edge, Drain, Queue, QueueFull, Poll,
  Dispatch, Route, Menu, DispatchEnd, Resync, Suppressed, Clamp, Clear, Alert };
struct TraceRecord {
  uint32_t us, event;
  int32_t raw, a, b, c, d;
  TraceKind kind;
};
template<size_t N> struct TraceBuffer {
  TraceRecord records[N]{};
  uint32_t count=0, generation=0;
  bool active=false, overflow=false;
  void start(){count=0;overflow=false;active=true;++generation;}
  inline __attribute__((always_inline)) void append(const TraceRecord& r){
    if(!active)return;
    if(count==N){overflow=true;active=false;return;}
    records[count++]=r;
  }
  void stop(){active=false;}
};
// ISR writes only this small internal-RAM staging queue. The UI task drains it
// into external RAM. Stop at either limit rather than silently losing evidence.
template<size_t N,size_t Limit> struct TraceStaging {
  TraceRecord records[N]{};
  uint32_t count=0,generation=0,head=0,tail=0,queued=0;
  bool active=false,overflow=false;
  uint8_t overflowReason=0; // 1=archive full, 2=UI could not drain staging in time
  void start(){count=head=tail=queued=0;overflow=false;overflowReason=0;active=true;++generation;}
  void stop(){active=false;}
  inline __attribute__((always_inline)) void append(const TraceRecord& r){
    if(!active)return;
    if(count==Limit || queued==N){overflow=true;active=false;overflowReason=count==Limit?1:2;return;}
    records[head]=r;head=(head+1)%N;++queued;++count;
  }
  bool pop(TraceRecord& r){if(!queued)return false;r=records[tail];tail=(tail+1)%N;--queued;return true;}
};
} }
