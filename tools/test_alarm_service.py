"""Compile real alarm catalog and queue integration against isolated boundaries."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Arduino.h','#pragma once\n#include <stdint.h>\nextern uint32_t nowMs;inline uint32_t millis(){return nowMs;}')
    write('core/store.h','''#pragma once
#include <stdint.h>
#include <map>
#include <string>
extern std::map<std::string,uint32_t> saved;extern bool writable,corrupt;extern int writes;
namespace core{namespace store{
inline uint32_t getU32(const char*n,const char*k,uint32_t d){auto key=std::string(n)+"/"+k;return saved.count(key)?saved[key]:d;}
inline uint8_t getU8(const char*n,const char*k,uint8_t d){return getU32(n,k,d);}
inline bool getBool(const char*n,const char*k,bool d){return getU32(n,k,d);}
inline bool putU32(const char*n,const char*k,uint32_t v){++writes;if(!writable)return false;saved[std::string(n)+"/"+k]=corrupt?v^1:v;return true;}
inline bool putU8(const char*n,const char*k,uint8_t v){return putU32(n,k,v);}
inline bool putBool(const char*n,const char*k,bool v){return putU32(n,k,v);}
}}
''')
    write('core/alert_vibration.h','#include <stdint.h>\nnamespace core{namespace alert_vibration{inline uint8_t validId(uint8_t v){return v<8?v:0;}}}')
    write('core/board_time.h','extern bool timezoneReady;namespace core{namespace time{inline bool timezoneConfigured(){return timezoneReady;}}}')
    write('services/morning_quote.h','#include <time.h>\nextern int quotes;namespace services{namespace morning_quote{inline void requestForEpoch(time_t){++quotes;}}}')
    write('services/automation.h','''#pragma once
#include <stdint.h>
#include <time.h>
#include <map>
#include "core/morning_alarm.h"
extern bool synced,accept;extern int schedules;
extern core::morning_alarm::Phase phase;
extern std::map<uint32_t,time_t> pending;
namespace services{namespace automation{
enum class Type{Red};enum class AlertSound:uint8_t{Classic,Chime,Digital,Gentle};enum class AlertKind{Standard,Morning};
inline bool wallClockSynced(){return synced;}
inline core::morning_alarm::Phase morningAlarmPhase(){return phase;}
inline bool cancelPendingWithServerId(uint32_t id){return pending.erase(id);}
inline bool hasPendingAutomationWithServerId(uint32_t id){return pending.count(id);}
inline bool ingestScheduledWithSound(Type,time_t t,const char*,uint32_t id,AlertSound,AlertKind,uint8_t){++schedules;if(accept)pending[id]=t;return accept;}
}}
''')
    write('main.cpp','''#include "services/alarms.h"
#include "core/store.h"
#include <cassert>
#include <cstdlib>
std::map<std::string,uint32_t> saved={{"alarmclk/en",1},{"alarmclk/h",8},{"alarmclk/m",15}};
std::map<uint32_t,time_t> pending={{99,500}};
bool synced=false,accept=true,timezoneReady=false,writable=true,corrupt=false;
int writes=0,schedules=0,quotes=0;uint32_t nowMs=0;
core::morning_alarm::Phase phase=core::morning_alarm::Phase::Idle;
time_t clockValue=1780000000;
extern "C" time_t time(time_t*p) {if(p)*p=clockValue;return clockValue;}
using namespace services::alarms;
int main(){
 setenv("TZ","UTC0",1);tzset();
 // An isolated read must not migrate the catalog, touch the snapshot or schedule.
 const auto before=snapshot();
 auto fresh=loadMorning();
 assert(fresh.hour==7&&fresh.minute==0&&!fresh.enabled&&fresh.vibration==0);
 assert(fresh.sound==services::automation::AlertSound::Classic);
 for(unsigned sound=0;sound<4;++sound)for(unsigned vibration=0;vibration<16;++vibration)
 for(unsigned enabled=0;enabled<2;++enabled){
  saved["morning/settings"]=23u|(59u<<8)|(sound<<16)|(vibration<<18)|(enabled<<24);
  fresh=loadMorning();
  assert(fresh.hour==23&&fresh.minute==59&&fresh.enabled==bool(enabled));
  assert(unsigned(fresh.sound)==sound&&fresh.vibration==(vibration<8?vibration:0));
 }
 saved["morning/settings"]=255u|(255u<<8);
 fresh=loadMorning();assert(fresh.hour==7&&fresh.minute==0);
 assert(writes==0&&schedules==0&&quotes==0&&pending.size()==1);
 assert(snapshot().count==before.count&&snapshot().morning.enabled==before.morning.enabled);
 saved.erase("morning/settings");reload();
 assert(snapshot().count==1&&snapshot().alarms[0].hour==8&&snapshot().alarms[0].minute==15);
 assert(saved["alarmclk/v2"]==1&&saved["alarmclk/count"]==1);
 int w=writes;reload();assert(writes==w); // migration is once only
 saved["alarmclk/h0"]=99;saved["alarmclk/m0"]=99;saved["alarmclk/s0"]=99;saved["alarmclk/v0"]=99;
 reload();assert(snapshot().alarms[0].hour==7&&snapshot().alarms[0].minute==0&&snapshot().alarms[0].vibration==0);
 nowMs=4000;poll();assert(schedules==0);
 synced=true;nowMs=8000;poll();assert(schedules==0); // timezone required too
 timezoneReady=true;nowMs=12000;poll();assert(schedules==1&&pending.size()==2);
 nowMs=13000;poll();assert(schedules==1);nowMs=16000;poll();assert(schedules==1);
 pending.erase(0xA10A0001u);accept=false;nowMs=20000;poll();assert(schedules==2&&pending.size()==1);
 accept=true;nowMs=24000;poll();assert(schedules==3&&pending.size()==2);
 Alarm disabled;disabled.enabled=false;assert(save(0,disabled));assert(pending.size()==1&&pending.count(99));
 assert(!save(8,disabled)&&!save(2,disabled));assert(save(1,disabled)&&snapshot().count==2);
 Alarm morning;morning.hour=9;morning.minute=30;
 writable=false;assert(!saveMorning(morning)&&!snapshot().morning.enabled);
 writable=true;corrupt=true;assert(!saveMorning(morning)&&!snapshot().morning.enabled);
 corrupt=false;assert(saveMorning(morning)&&snapshot().morningScheduled&&quotes==1);
 assert(pending.count(0xA10B0001u)&&pending[0xA10B0001u]>clockValue);
 time_t firstFire=pending[0xA10B0001u];
 clockValue=firstFire;pending.erase(0xA10B0001u);nowMs=28000;poll();
 assert(pending[0xA10B0001u]>firstFire&&quotes==2);
 phase=core::morning_alarm::Phase::Ringing;assert(saveMorning(morning)&&quotes==2);
 prepareNextMorningQuote();assert(quotes==3); // explicit routine handoff request
 morning.enabled=false;assert(saveMorning(morning));assert(!pending.count(0xA10B0001u));
 nowMs=32000;poll();assert(!snapshot().morningScheduled&&pending.count(99));
 saved["alarmclk/count"]=99;reload();assert(snapshot().count==kMaxAlarms);
}
''')
    binary=root/'test'
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/alarms.cpp'),str(root/'main.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS fresh isolated Morning reads, migration, validation, clock/timezone gates, cadence, retry/dedupe, private cancellation and Morning save/quote behavior')
