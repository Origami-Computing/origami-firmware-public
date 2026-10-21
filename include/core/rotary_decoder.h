#pragma once
#include <stdint.h>

namespace core { namespace rotary {
// Incremental A/B decoding. Counters are unsigned and may wrap. Caller owns
// synchronization; no allocation, I/O or timers. Positive counts match UI down.
struct Scroll {
  int8_t delta;bool undo;uint32_t gesture;
  Scroll(int8_t d=0,bool u=false,uint32_t g=0):delta(d),undo(u),gesture(g){}
};
struct Decoder {
  static constexpr int kTransitionsPerScroll=4;
  static constexpr int kTriggerTransitions=3;
  // COM20 wheel rests at A=1, B=0 (confirmed by physical reversal traces).
  static constexpr uint8_t kRestState=2;
  uint8_t state=0;
  bool anchored=false;
  int8_t partial=0,early=0;
  uint32_t gesture=0,eventDrops=0;
  static constexpr unsigned kQueueSize=32;
  Scroll events[kQueueSize];
  uint8_t head=0,tail=0,count=0;
  inline __attribute__((always_inline)) void emit(int direction,bool undo){
    pending+=direction;
    if(direction<0)++scrollForward;else ++scrollReverse;
    if(count==kQueueSize){++eventDrops;return;}
    events[head]=Scroll(-direction,undo,gesture);head=(head+1)%kQueueSize;++count;
  }
  bool takeScroll(Scroll& out){
    if(!count){pending=0;return false;}
    out=events[tail];tail=(tail+1)%kQueueSize;--count;pending+=out.delta;return true;
  }
  int32_t pending=0;
  uint32_t forward=0,reverse=0,invalid=0,resyncs=0,scrollForward=0,scrollReverse=0;
  inline __attribute__((always_inline)) void step(uint8_t next) {
    next &= 3;
    const uint8_t changed=state^next;
    if(!changed)return;
    const int delta=((state&1) ^ (next>>1)) ? 1 : -1;
    state=next;
    if(changed==3){
      ++invalid;
      if(early)emit(-early,true);
      early=0;partial=0;
      // Direction is unknowable across a two-bit jump. Discard this cycle,
      // then establish a fresh origin at the next observed resting state.
      anchored=next==kRestState;
      return;
    }
    if(delta<0)++forward;else ++reverse;
    if(!anchored){
      partial=0;
      if(next==kRestState)anchored=true;
      return;
    }
    partial+=delta;
    if(!early && (partial==kTriggerTransitions || partial==-kTriggerTransitions)){
      early=partial>0?1:-1;if(!++gesture)++gesture;emit(early,false);
    }
    if(next==kRestState){
      // Full +/-4 confirms the provisional scroll without emitting again.
      // A return to zero abandons the excursion and rolls it back once.
      if(partial==0 && early)emit(-early,true);
      early=0;
      // Partial excursions and bounce returning to rest cannot leave an offset.
      partial=0;
    }
  }
  void resync(uint8_t next){state=next&3;anchored=state==kRestState;partial=early=0;pending=0;head=tail=count=0;++resyncs;}
  int take(){const int result=-pending;pending=0;head=tail=count=0;return result;}
};
} }
