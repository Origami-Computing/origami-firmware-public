"""Compile the production event encoder and validate its actual wire payloads."""
from pathlib import Path
import argparse,json,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
a=argparse.ArgumentParser();a.add_argument('--wire-output',type=Path);args=a.parse_args()
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 (p/'esp_random.h').write_text('#pragma once\ninline unsigned esp_random(){static unsigned x=1;return x++;}\n')
 (p/'esp_timer.h').write_text('#pragma once\n#include <stdint.h>\ninline int64_t esp_timer_get_time(){return 9000000000000000LL;}\n')
 (p/'main.cpp').write_text(r'''
#include "core/usage_events.h"
#include "core/telemetry_queue.h"
#include <cstdio>
#include <cassert>
#include <cstring>
namespace core {namespace telemetry_queue {
void enqueue(Kind k,const char* json){assert(k==KIND_USAGE);assert(strlen(json)<kItemJsonCap);puts(json);}
}}
int main(){
 core::usage::emit("music","mp3.start");
 core::usage::init();
 core::usage::emit("music","mp3.start");
 core::usage::emit("camera","camera.saved");
 core::usage::emit("agent","chat.ready",1200);
 core::usage::emit("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv","app.exit",9000000000000LL);
 core::usage::emit("bad\\\"private","app.enter");
}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(root/'include'),str(root/'src/core/usage_events.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 result=subprocess.check_output([str(p/'test')],text=True)
 events=[json.loads(line) for line in result.splitlines()]
 assert len(events)==4 and [e['seq'] for e in events]==[0,1,2,3]
 assert all(len(e['boot'])==32 and e['v']==1 for e in events)
 assert events[2]['duration_ms']==1200 and events[3]['duration_ms']==9000000000000
 if args.wire_output: args.wire_output.write_text(json.dumps(events,indent=2)+'\n')
 print('PASS actual firmware encoder: identity, sequencing, long durations, maximum app identifier, startup and escaping')
