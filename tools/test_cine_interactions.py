"""Compile the actual Cine/Home handlers with recording navigation and haptics."""

from pathlib import Path
import subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    s = (ROOT / path).read_text()
    a = s.index(signature)
    b = s.index("{", a)
    depth = 1
    end = b + 1
    while depth:
        depth += (s[end] == "{") - (s[end] == "}")
        end += 1
    return s[a:end]


code = (
    r"""
#include <cassert>
#include <stdint.h>
#include "core/cine_controls.h"
#include "services/vibration.h"
namespace hal{namespace input{enum class EventType{ButtonA_Tap,ButtonB_Tap,ButtonC_Tap,Scroll,Other};struct InputEvent{EventType type;int delta=0;};}}
unsigned cues=0,suppressed=0;services::vibration::Cue last=services::vibration::Cue::None;
namespace services{void notifyUserActivity(){}namespace vibration{void navigationFeedback(Cue c){++cues;last=c;}void suppressInputFeedback(){++suppressed;}}}
struct App{};
namespace apps{namespace cine_camera{App app;}namespace camera_menu{App app;}namespace voice_recorder{App app;}namespace menu{App app;}}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}App* pushed=nullptr;int popped=0;void push(App* a){pushed=a;}void pop(){++popped;}};
namespace ui{namespace nav{bool isBackTap(hal::input::InputEvent e){return e.type==hal::input::EventType::ButtonA_Tap;}bool isEnterTap(hal::input::InputEvent e){return e.type==hal::input::EventType::ButtonC_Tap;}}}
namespace services { namespace camera {
enum class CaptureResolution { Vga, Hd };
enum class Setting { Capture };
} }
struct { services::camera::CaptureResolution capture=services::camera::CaptureResolution::Vga;
  void save(services::camera::Setting) {} } cameraSettings;
bool microphone=true, clipActive=false;
struct List{int selected=0,totalItems=3;void scroll(int delta){selected+=delta;if(selected<0)selected=0;if(selected>2)selected=2;}}settingsList;
struct Session{struct Status{bool open=true;}s;int shutter=0,wb=0;void adjustShutter(int d){shutter+=d;}void adjustColor(int d){wb+=d;}int autoResumes=0;bool resumeAutoExposure(){++autoResumes;return s.open;}const Status& status(){return s;}}session;
enum class View{Menu,Opening,Preview,Error};View current=View::Preview;
bool menuEditing=false;core::cine::WheelControl wheel;unsigned clockMs=100,photos=0,opens=0;uint32_t millis(){return clockMs;}void invalidate(){}void begin(){++opens;}void snap(){++photos;}
"""
    + function("src/apps/cine_camera_app.cpp", "void input(hal::input::InputEvent e)")
    + function("src/apps/cine_camera_app.cpp", "bool holdWheel()")
    + r"""
struct AgentState{};
"""
    + function(
        "src/apps/agent/screen_eyes.cpp",
        "void onInput(AgentState& s, hal::input::InputEvent ev)",
    )
    + r"""
int main(){using E=hal::input::EventType;
 assert(wheel.color()&&!wheel.visible());
 input({E::Scroll,1});assert(session.wb==1&&session.shutter==0&&cues==0);
 input({E::ButtonC_Tap});assert(!wheel.color()&&cues==1&&last==services::vibration::Cue::Select);
 input({E::Scroll,3});assert(session.shutter==-3&&session.wb==1&&cues==1);
 clockMs=1000;input({E::ButtonC_Tap});assert(wheel.color()&&cues==2&&wheel.shown(2999)&&!wheel.shown(3000));
 input({E::ButtonB_Tap});assert(photos==1);
 input({E::ButtonA_Tap});assert(current==View::Menu&&settingsList.selected==1&&!menuEditing);
 input({E::Scroll,-1});assert(settingsList.selected==0);
 input({E::ButtonC_Tap});assert(menuEditing&&cues==2);input({E::Scroll,1});assert(session.shutter==-4);
 input({E::ButtonA_Tap});assert(!menuEditing&&current==View::Menu);input({E::ButtonA_Tap});assert(current==View::Preview);
 current=View::Opening;input({E::ButtonC_Tap});assert(cues==2);current=View::Error;input({E::ButtonA_Tap});assert(NavHandler::instance().popped==1);
 current=View::Preview;assert(wheel.color());assert(holdWheel()&&session.autoResumes==1&&!wheel.color()&&wheel.shown(clockMs)&&cues==3);
 current=View::Menu;menuEditing=true;settingsList.selected=1;assert(holdWheel()&&session.autoResumes==2&&!menuEditing&&settingsList.selected==0);
 current=View::Opening;session.s.open=false;assert(holdWheel()&&session.autoResumes==2);
 AgentState state;onInput(state,{E::ButtonB_Tap});assert(NavHandler::instance().pushed==&apps::cine_camera::app);
 onInput(state,{E::ButtonA_Tap});assert(NavHandler::instance().pushed==&apps::voice_recorder::app);
 onInput(state,{E::Scroll,1});assert(NavHandler::instance().pushed==&apps::menu::app);
 onInput(state,{E::ButtonC_Tap});assert(suppressed==1);
}
"""
)
assert (
    '{"Camera", &apps::cine_camera::app' in (ROOT / "src/apps/menu_app.cpp").read_text()
)
with tempfile.TemporaryDirectory(prefix="cine-interactions-") as td:
    p = Path(td)
    (p / "test.cpp").write_text(code)
    subprocess.run(
        [
            "c++",
            "-std=c++17",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-I" + str(ROOT / "include"),
            str(p / "test.cpp"),
            "-o",
            str(p / "test"),
        ],
        check=True,
    )
    subprocess.run([str(p / "test")], check=True)
print(
    "PASS actual Cine/Home input: Select haptic per mode switch, wheel routing, 2s hold, settings/capture, Home B â†’ Cine, Home and main menu open the same Camera; ASan/UBSan"
)
