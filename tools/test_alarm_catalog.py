"""Compare the catalog against frozen pre-extraction callers (no Git history).

Preserves distinct invalid-ID policies: Alarm/Timer preview silence and Classic
labels/ringing; interval feedback uses Gentle; Pomodoro labels wrap modulo four.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
BASELINE=ROOT/'test/fixtures/alarm_catalog_baseline'
def block(source,marker):
 a=source.index(marker);b=source.index('{',a)+1;depth=1
 while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
old={}
for item in json.loads((BASELINE/'manifest.json').read_text())['files']:
 data=(BASELINE/item['path']).read_bytes()
 if hashlib.sha256(data).hexdigest()!=item['sha256']:
  raise RuntimeError('Frozen alarm reference changed: '+item['path'])
 old[item['name']]=data.decode()
code='''#include "sfx/sfx_lib.h"
#include "sfx/alarm_catalog.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int played=-1;
namespace services {namespace automation {enum class AlertSound:uint8_t {Classic,Chime,Digital,Gentle};}}
'''
enum=block((ROOT/'include/services/automation.h').read_text(),'enum class AlertSound')+';'
code=code.replace('enum class AlertSound:uint8_t {Classic,Chime,Digital,Gentle};',enum)
code+='static_assert((int)services::automation::AlertSound::Classic==0 && (int)services::automation::AlertSound::Chime==1 && (int)services::automation::AlertSound::Digital==2 && (int)services::automation::AlertSound::Gentle==3,"Persisted sound IDs changed");\n'
for name in ['timer','alarm']:
 code+='namespace '+name+'{'+block(old[name],'static const char* soundName(')+block(old[name],'static void previewSound(')+'}\n'
code+='namespace ring {'+block(old['ring'],'static const SfxDef& ringDef(')+block(old['ring'],'static uint16_t ringGapMs(')+'}\n'
code+='namespace interval {'+block(old['interval'],'const SfxDef& definition(')+'}\n'
code+='''int main(){
 const char* names[]={"Daybreak","Sunrise","Morning Steps","Gentle Bells"};
 const int gaps[]={1050,1150,900,1300};
 for(unsigned id=0;id<256;++id){
  auto sound=static_cast<services::automation::AlertSound>(id);
  const auto& p=sfx::alarms::preset(id);
  assert(!strcmp(p.name,timer::soundName(sound))&&!strcmp(p.name,alarm::soundName(sound)));
  assert(p.ring.id==ring::ringDef(sound).id&&p.repeatGapMs==ring::ringGapMs(sound));
  played=-1;timer::previewSound(sound);int expected=played;
  played=-1;alarm::previewSound(sound);assert(played==expected);
  played=-1;sfx::alarms::preview(id);assert(played==expected);
  const auto& feedback=sfx::alarms::preset(id<4?id:3);
  assert(feedback.preview.id==interval::definition(id,true).id);
  assert(feedback.ring.id==interval::definition(id,false).id);
  assert(feedback.repeatGapMs==gaps[id<4?id:3]);
  assert(!strcmp(sfx::alarms::preset(id%4).name,names[id%4]));
 }
 puts("PASS 256 IDs: labels, preview silence, ring definitions/gaps, interval fallback and Pomodoro wrap");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'sfx').mkdir()
 (p/'sfx/sfx_lib.h').write_text('''#pragma once
#include <stdint.h>
struct SfxDef{int id;};extern int played;
namespace sfx {
inline constexpr SfxDef kSfxAlarmClassicPreview{10},kSfxAlarmChimePreview{11},kSfxAlarmDigitalPreview{12},kSfxAlarmGentlePreview{13};
inline constexpr SfxDef kSfxAlarmClassicRing{0},kSfxAlarmChimeRing{1},kSfxAlarmDigitalRing{2},kSfxAlarmGentleRing{3};
inline void play(const SfxDef& d){played=d.id;}
}
''')
 (p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),str(ROOT/'src/sfx/alarm_catalog.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
