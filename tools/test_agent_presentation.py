"""Check real Agent presentation policy and default privileges on reused storage."""
from pathlib import Path
import subprocess
import argparse
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-ref', help='Check a historical implementation as a negative control')
args = parser.parse_args()
def read(name):
    return subprocess.check_output(['git','show',args.source_ref+':'+name],cwd=ROOT,text=True) if args.source_ref else (ROOT/name).read_text()
source = read('src/apps/agent_app.cpp')
start = source.index('static AppPresentation presentationPolicy()')
end = source.index('static core::health::RuntimePolicy runtimePolicy()', start)
policy = source[start:end]
boot = read('src/apps/boot_app.cpp')
boot_start = boot.index('static AppPresentation presentationPolicy()')
boot_policy = boot[boot_start:boot.index('App app =', boot_start)]
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'app.h').write_text(read('include/app.h'))
    (path/'check.cpp').write_text(r'''
#include "app.h"
#include <cassert>
#include <cstring>
#include <new>
#include <cstdio>
class TFT_eSprite {};
namespace motion { bool owns=false; bool ownsStatusBackground(){return owns;} }
App app{};
struct NavHandler {
    App* foreground=&app;
    static NavHandler& instance(){static NavHandler n;return n;}
    App* top(){return foreground;}
};
bool recording=false; int overlays=0;
bool isRecording(){return recording;}
void drawRecordingOverlay(){++overlays;}
''' + policy + '\nnamespace boot {\n' + boot_policy + '}\n' + r'''
int main(){
    // A descriptor constructed over former stack/storage bytes must never
    // inherit a hidden/transparent mode or a stale callback from those bytes.
    alignas(AppPresentation) unsigned char storage[sizeof(AppPresentation)];
    for(int byte=0;byte<256;++byte){
        memset(storage,byte,sizeof(storage));
        auto* p = new(storage) AppPresentation;
        assert(!p->contentUsesStatusArea && !p->hideAppOverlays);
        assert(p->statusBar==AppPresentation::StatusBar::Default);
        assert(p->drawOverlay==nullptr);
        p->~AppPresentation();
    }
    const auto bootPolicy = boot::presentationPolicy();
    assert(bootPolicy.contentUsesStatusArea);
    assert(bootPolicy.statusBar==AppPresentation::StatusBar::Default);
    assert(!bootPolicy.hideAppOverlays && !bootPolicy.drawOverlay);
    TFT_eSprite surface;
    for(int iteration=0;iteration<100;++iteration){
        motion::owns=true;
        auto animated=presentationPolicy();
        assert(animated.statusBar==AppPresentation::StatusBar::GlyphOverlay);
        assert(!animated.contentUsesStatusArea && !animated.hideAppOverlays);
        recording=true;animated.drawOverlay(surface);assert(overlays==iteration+1);
        NavHandler::instance().foreground=nullptr;
        animated.drawOverlay(surface);assert(overlays==iteration+1);
        NavHandler::instance().foreground=&app;
        motion::owns=false;
        auto home=presentationPolicy();
        assert(home.statusBar==AppPresentation::StatusBar::Default);
        assert(!home.contentUsesStatusArea && !home.hideAppOverlays);
        recording=false;home.drawOverlay(surface);assert(overlays==iteration+1);
    }
    puts("PASS deterministic default privileges and real Agent animated-to-Home policy");
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),
                    str(path/'check.cpp'),'-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True)
