#include "core/alert_vibration.h"
#include "core/store.h"
#include <stdio.h>
#include <string.h>

namespace core { namespace alert_vibration {
namespace {
const Pattern presets[kPresetCount]={
 {1,1,"Classic",{{100,20}}},
 {1,1,"Soft reminder",{{350,2650}}},
 {1,2,"Double tap",{{180,180},{260,3000}}},
 {1,1,"Long wave",{{1400,4200}}},
 {1,1,"Breathing",{{2400,3600}}},
 {1,3,"Three steps",{{200,250},{250,250},{300,4000}}},
 {1,2,"Heartbeat",{{120,140},{260,4200}}},
 {1,1,"Patient nudge",{{450,4550}}},
 {1,2,"Long-short",{{600,220},{180,3800}}},
 {1,1,"Dawn pulses",{{900,5100}}}
};
const Pattern off={1,0,"Off",{}};
Pattern custom[kCustomCount];
bool loaded=false;
void load() {
  if(loaded)return;
  for(uint8_t i=0;i<kCustomCount;++i) {
    char key[8]; snprintf(key,sizeof(key),"slot%u",i);
    Pattern p;
    if(core::store::getBlob("vibration",key,&p,sizeof(p)) && valid(p)) custom[i]=p;
    else { custom[i]=presets[1]; snprintf(custom[i].name,sizeof(custom[i].name),"Custom %u",i+1); }
  }
  loaded=true;
}
}
uint8_t validId(uint8_t id) { return id<kCount?id:0; }
const Pattern& pattern(uint8_t id) {
  id=validId(id); if(id<kPresetCount)return presets[id];
  if(id==kOff)return off;
  load(); return custom[id-kPresetCount];
}
const char* name(uint8_t id) { return pattern(id).name; }
uint32_t cycleMs(const Pattern& p) {
  uint32_t total=p.leadMs;
  for(uint8_t i=0;i<p.count && i<kMaxPulses;++i) total+=p.pulses[i].onMs+p.pulses[i].offMs;
  return total;
}
bool valid(const Pattern& p) {
  if(p.version!=1 || !p.count || p.count>kMaxPulses || !p.name[0] || p.name[20] || p.leadMs>10000)return false;
  for(const char* s=p.name;*s;++s) if(*s<32 || *s>126)return false;
  for(uint8_t i=0;i<p.count;++i)
    if(p.pulses[i].onMs<20 || p.pulses[i].onMs>5000 || p.pulses[i].offMs>10000)return false;
  return cycleMs(p)<=30000;
}
bool activeAt(const Pattern& p,uint32_t elapsed) {
  const uint32_t total=cycleMs(p); if(!total)return false;
  uint32_t phase=elapsed%total;
  if(phase<p.leadMs)return false;
  phase-=p.leadMs;
  for(uint8_t i=0;i<p.count;++i) {
    if(phase<p.pulses[i].onMs)return true;
    phase-=p.pulses[i].onMs;
    if(phase<p.pulses[i].offMs)return false;
    phase-=p.pulses[i].offMs;
  }
  return false;
}
bool saveCustom(uint8_t slot,const Pattern& p) {
  if(slot>=kCustomCount || !valid(p))return false;
  load(); char key[8]; snprintf(key,sizeof(key),"slot%u",slot);
  Pattern check;
  if(!core::store::putBlob("vibration",key,&p,sizeof(p)) ||
     !core::store::getBlob("vibration",key,&check,sizeof(check)) || memcmp(&p,&check,sizeof(p)))return false;
  custom[slot]=check; return true;
}
} }
