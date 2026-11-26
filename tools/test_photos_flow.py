"""Exercise the shared Photos control state machine (include/apps/photos_flow.h).

The firmware app and Kami Screen Studio's Photos demo both drive this reducer.
Uses $CXX, else c++, else `python -m ziglang c++`.
"""
from pathlib import Path
import os, shlex, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
SOURCE = r'''#include "apps/photos_flow.h"
#include <cassert>
#include <cstdio>
using namespace apps::photos;
using namespace apps::photos::flow;
static const Item photo{false,false}, uploadedPhoto{false,true}, video{true,false};
int main(){
 Model m; loaded(m, 5);
 assert(m.mode==Mode::Browse && m.index==0);
 // Turning browses and wraps; a zero delta does nothing.
 assert(apply(m,Input::Turn,1,photo,true)==Command::Browse && m.index==1);
 assert(apply(m,Input::Turn,-2,photo,true)==Command::Browse && m.index==4);
 assert(apply(m,Input::Turn,0,photo,true)==Command::None);
 // RB plays the video under the cursor and does nothing on a photo.
 assert(apply(m,Input::Right,0,video,true)==Command::Play && m.mode==Mode::Browse);
 assert(apply(m,Input::Right,0,photo,true)==Command::None && m.mode==Mode::Browse);
 // C opens the sheet for videos (Play, Upload, Delete) and photos (Upload, Delete).
 assert(apply(m,Input::Press,0,video,true)==Command::OpenSheet && m.sheet.count==3 && m.focused()==policy::Action::Play);
 assert(apply(m,Input::Back,0,video,true)==Command::RedrawImage && m.mode==Mode::Browse);
 assert(apply(m,Input::Press,0,photo,true)==Command::OpenSheet && m.mode==Mode::Sheet);
 assert(m.sheet.count==2 && m.focused()==policy::Action::Upload);
 // RB is inert in the sheet; turning never browses.
 assert(apply(m,Input::Right,0,photo,true)==Command::None && m.mode==Mode::Sheet);
 assert(apply(m,Input::Turn,-5,photo,true)==Command::None && m.row==0 && m.index==4);
 assert(apply(m,Input::Turn,9,photo,true)==Command::Repaint && m.focused()==policy::Action::Delete);
 // A press before the focus change is drawn is ignored.
 assert(apply(m,Input::Press,0,photo,false)==Command::None && m.mode==Mode::Sheet);
 // Delete asks to confirm with Cancel focused; Cancel returns to the sheet.
 assert(apply(m,Input::Press,0,photo,true)==Command::RedrawImage && m.mode==Mode::Confirm && !m.confirmDelete);
 assert(apply(m,Input::Press,0,photo,true)==Command::RedrawImage && m.mode==Mode::Sheet);
 // LB backs out of the confirmation to the sheet; RB is inert there.
 apply(m,Input::Press,0,photo,true); assert(m.mode==Mode::Confirm);
 assert(apply(m,Input::Right,0,photo,true)==Command::None && m.mode==Mode::Confirm);
 assert(apply(m,Input::Back,0,photo,true)==Command::RedrawImage && m.mode==Mode::Sheet);
 // Turning clockwise reaches Delete, which requests the effect.
 apply(m,Input::Press,0,photo,true);
 assert(apply(m,Input::Turn,1,photo,true)==Command::Repaint && m.confirmDelete);
 assert(apply(m,Input::Press,0,photo,true)==Command::Delete);
 removed(m, 4); assert(m.mode==Mode::Browse && m.index==3 && m.count==4);
 // An uploaded photo still offers Upload (re-upload) before Delete.
 assert(apply(m,Input::Press,0,uploadedPhoto,true)==Command::OpenSheet && m.sheet.count==2);
 assert(m.focused()==policy::Action::Upload);
 apply(m,Input::Back,0,photo,true);
 // Upload only queues: the sheet closes and browsing continues immediately.
 apply(m,Input::Press,0,photo,true);
 assert(apply(m,Input::Press,0,photo,true)==Command::Upload);
 uploadQueued(m); assert(m.mode==Mode::Browse);
 assert(apply(m,Input::Turn,1,photo,true)==Command::Browse);
 // Failed delete/upload return to the sheet.
 apply(m,Input::Press,0,photo,true); uploadFailed(m); assert(m.mode==Mode::Sheet);
 deleteFailed(m); assert(m.mode==Mode::Sheet && m.row==0);
 // LB leaves Photos from browsing; deleting the last item empties the gallery.
 apply(m,Input::Back,0,photo,true);
 assert(apply(m,Input::Back,0,photo,true)==Command::Exit);
 Model one; loaded(one,1); removed(one,0); assert(one.mode==Mode::Empty);
 assert(apply(one,Input::Press,0,photo,true)==Command::Reload);
 assert(apply(one,Input::Turn,1,photo,true)==Command::None);
 puts("PASS");
}
'''


def compiler():
    if os.environ.get("CXX"):
        return shlex.split(os.environ["CXX"].replace("\\", "/"))
    if shutil.which("c++"):
        return ["c++"]
    return [sys.executable, "-m", "ziglang", "c++"]


with tempfile.TemporaryDirectory() as td:
    folder = Path(td)
    (folder / "main.cpp").write_text(SOURCE)
    exe = folder / ("test.exe" if os.name == "nt" else "test")
    subprocess.run(compiler() + ["-std=c++11", "-Wall", "-Wextra", "-I" + str(root / "include"),
                                 str(folder / "main.cpp"), "-o", str(exe)], check=True,
                   stderr=subprocess.DEVNULL if "ziglang" in " ".join(compiler()) or "zig" in os.environ.get("CXX", "") else None)
    output = subprocess.check_output([str(exe)], text=True)
    assert output.strip() == "PASS", output
print("PASS Photos flow: browse, RB play, sheet, confirm, queue-only upload, failures, exit and empty gallery")
