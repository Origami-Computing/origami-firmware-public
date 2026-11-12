"""Exercise production vibration ownership/preview with fake clock, NVS and motor.

This characterizes existing last-writer behavior, not physical pulse timing or
exclusive hardware arbitration. Hardware diagnostics can bypass this owner.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="kami-vibration-") as directory:
    root = Path(directory)
    def write(name, text):
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    write("Arduino.h", "#include <stdint.h>\n#include <stdio.h>\nuint32_t millis();\n")
    write("hal/hal_io.h", "namespace hal { namespace io { void setVibration(bool); } }\n")
    write("core/store.h", "#include <stddef.h>\nnamespace core { namespace store { bool getBlob(const char*,const char*,void*,size_t); bool putBlob(const char*,const char*,const void*,size_t); } }\n")
    write("services/haptic_playback.h", "#pragma once\n#include <cstdint>\n#include \"core/haptic_effect.h\"\nnamespace services { namespace haptic_playback { struct Status {bool playing=false; uint32_t maxGapUs=0,transitions=0;}; inline void stop(bool=false){} inline bool start(const core::haptic::Effect&){return false;} inline bool startLoop(const core::haptic::Effect&,unsigned=4000){return false;} inline void renewLoop(){} inline Status status(){return Status{};} }}\n")
    write("test.cpp", r'''
#include "services/vibration.h"
#include "hal/hal_io.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <vector>
using namespace core::alert_vibration;
using namespace services::vibration;
static uint32_t now;
static std::vector<bool> writes;
static Pattern persisted{};
static bool readable=false, writable=true, corrupt=false;
static bool savedHaptics=true, hapticsSaved=false;
uint32_t millis(){return now;}
namespace hal { namespace io { void setVibration(bool value){writes.push_back(value);} } }
namespace core { namespace store {
bool getBlob(const char* space,const char* key,void* value,size_t size){
 if(!strcmp(key,"haptics")){assert(size==sizeof(bool));if(!hapticsSaved)return false;memcpy(value,&savedHaptics,size);return true;}
 assert(!strcmp(space,"vibration") && size==sizeof(Pattern));
 if(!readable)return false;
 memcpy(value,&persisted,size);
 if(corrupt)static_cast<Pattern*>(value)->name[0]='X';
 return true;
}
bool putBlob(const char* space,const char* key,const void* value,size_t size){
 if(!strcmp(key,"haptics")){assert(size==sizeof(bool));if(!writable)return false;memcpy(&savedHaptics,value,size);hapticsSaved=true;return true;}
 assert(!strcmp(space,"vibration") && size==sizeof(Pattern));
 if(!writable)return false;
 memcpy(&persisted,value,size);return true;
}
} }
int main(){
 // Existing last-writer ownership is conditional release, not a priority lease.
 drive(Owner::Alarm,0,0);assert(writes.back());auto n=writes.size();
 release(Owner::Pomodoro);assert(writes.size()==n);
 drive(Owner::Pomodoro,0,100);assert(!writes.back());n=writes.size();
 release(Owner::Alarm);assert(writes.size()==n);
 release(Owner::Pomodoro);assert(writes.size()==n+1 && !writes.back());
 drive(Owner::Alarm,0,0);assert(writes.back());
 hal::io::setVibration(false);n=writes.size(); // Hardware Test bypasses bookkeeping.
 release(Owner::Alarm);assert(writes.size()==n+1 && !writes.back());

 Pattern p{1,1,"Test",{{100,20}},20};assert(valid(p));
 assert(cycleMs(p)==140 && !activeAt(p,19) && activeAt(p,20));
 assert(activeAt(p,119) && !activeAt(p,120) && !activeAt(p,139));
 assert(!activeAt(p,140) && activeAt(p,160));
 now=1000;n=writes.size();assert(previewCustom(p,200) && previewing());
 assert(writes.size()==n); // Scheduling itself does not drive the motor.
 servicePreview(1019,true);assert(!writes.back());
 servicePreview(1020,true);assert(writes.back());
 servicePreview(1199,true);assert(previewing() && writes.back());
 servicePreview(1200,true);assert(!previewing() && !writes.back());

 assert(preview(0,100));servicePreview(now,true);assert(writes.back());
 Pattern bad=p;bad.version=0;
 n=writes.size();assert(!previewCustom(bad,100) && !previewCustom(p,99));
 assert(!previewCustom(p,10001) && !preview(kCount,100));
 assert(previewing() && writes.size()==n); // Invalid request preserves active preview.
 drive(Owner::Alarm,0,0);n=writes.size();
 servicePreview(now,false);assert(!previewing() && writes.size()==n);
 release(Owner::Alarm);assert(!writes.back());
 assert(preview(0,100));servicePreview(now,true);assert(writes.back());
 assert(preview(kOff,0));assert(!previewing() && !writes.back());

 now=UINT32_MAX-50;assert(preview(0,200));
 servicePreview(49,true);assert(previewing() && !writes.back()); // elapsed 100
 servicePreview(69,true);assert(previewing() && writes.back()); // elapsed 120
 servicePreview(149,true);assert(!previewing() && !writes.back()); // elapsed 200
 now=0;assert(preview(0,100));drive(Owner::Pomodoro,0,0);
 stopAll();assert(!previewing() && !writes.back());n=writes.size();
 release(Owner::Pomodoro);assert(writes.size()==n);

 // Storage verification must not publish a failed custom-pattern update.
 assert(!strcmp(name(kPresetCount),"Custom 1"));
 writable=false;assert(!saveCustom(0,p));assert(!strcmp(name(kPresetCount),"Custom 1"));
 writable=true;readable=false;assert(!saveCustom(0,p));
 readable=true;corrupt=true;assert(!saveCustom(0,p));
 assert(!strcmp(name(kPresetCount),"Custom 1"));
 corrupt=false;assert(saveCustom(0,p));assert(!strcmp(name(kPresetCount),"Test"));
 assert(!saveCustom(kCustomCount,p) && !saveCustom(0,bad));
 assert(!strcmp(name(kPresetCount),"Test"));
 // Settings > Haptics: default on; off refuses feedback haptics but never alerts.
 stopAll();now=5000;
 assert(hapticsEnabled());
 assert(pulse(Owner::Golf,50) && writes.back());
 writable=false;assert(!setHapticsEnabled(false) && hapticsEnabled() && writes.back()); // failed save keeps it on
 writable=true;n=writes.size();
 assert(setHapticsEnabled(false) && !hapticsEnabled() && savedHaptics==false);
 assert(writes.size()==n+1 && !writes.back()); // a running game pulse stops at once
 n=writes.size();
 assert(!pulse(Owner::Golf,50) && !pulse(Owner::Tennis,50) && !pulse(Owner::Bowling,50) && writes.size()==n);
 navigationFeedback(Cue::Select);dizzyFeedback(true);dizzyFeedback(false);
 assert(!widgetCue("complete") && writes.size()<=n+1);
 drive(Owner::Alarm,0,0);assert(writes.back()); // alarms still vibrate
 release(Owner::Alarm);assert(!writes.back());
 assert(preview(0,100));servicePreview(now,true);assert(writes.back()); // explicit previews too
 stopPreview();assert(!writes.back());
 assert(setHapticsEnabled(true) && hapticsEnabled() && savedHaptics==true);
 assert(pulse(Owner::Golf,50) && writes.back());release(Owner::Golf);
 puts("PASS haptics setting: default on, failed save kept, off silences games/navigation/Lua but not alarms or previews");
 puts("PASS production vibration ownership, raw diagnostic bypass, preview timing/wrap/cancellation, failed persistence publication");
}
''')
    source = (ROOT / "src/services/vibration.cpp").read_text()
    for name, code, expected in [
        ("production", source, True),
        ("release-negative-control", source.replace("if(owner==who)", "if(true)"), False),
    ]:
        write("implementation.cpp", code)
        binary = root / name
        subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                        "-I" + str(root), "-I" + str(ROOT / "include"),
                        str(root / "test.cpp"), str(ROOT / "src/core/alert_vibration.cpp"), str(root / "implementation.cpp"),
                        "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if expected:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and "Assertion" in result.stderr, result.stderr
            print("PASS broken owner-release negative control rejected")
