"""Exercise production native navigation composition and deferred code retirement."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'scene').mkdir()
 (p/'app.h').write_text('''#pragma once
namespace scene {struct Screen;}
struct App {const char* name;const scene::Screen* initialScreen;void (*onExit)();};
''')
 (p/'scene/screen.h').write_text('''#pragma once
namespace scene {struct Screen {};}
''')
 (p/'nav_handler.h').write_text('''#pragma once
#include "app.h"
struct NavHandler {static NavHandler& instance();bool open(App*);};
''')
 (p/'main.cpp').write_text('''
#include "app.h"
#include "scene/screen.h"
#include "nav_handler.h"
#include "control/native_apps.h"
#include "services/native_apps.h"
#include <cassert>
namespace {bool ready=true,active=false,retired=false,inCallback=false,deny=false;int exits=0,frees=0;
scene::Screen screen;void original(){assert(inCallback);++exits;}App game{"Game",&screen,original};}
NavHandler& NavHandler::instance(){static NavHandler n;return n;}
bool NavHandler::open(App* app){assert(app==&game);return !deny;}
namespace services {namespace native_apps {
bool activate(void*& out){if(!ready)return false;ready=false;active=true;out=&game;return true;}
void retire(){assert(active);retired=true;}
void collect(){if(retired){assert(!inCallback);++frees;active=retired=false;}}
}}
int main(){
 assert(control::native_apps::openPrepared());assert(active);
 // This is the dangerous case: module input calls pop and is still on stack.
 inCallback=true;game.onExit();assert(exits==1&&retired&&frees==0);
 inCallback=false;control::native_apps::collect();assert(frees==1&&!active);
 game.onExit=original;ready=true;deny=true;
 assert(!control::native_apps::openPrepared());assert(retired&&game.onExit==original);
 control::native_apps::collect();assert(frees==2&&exits==1);
 ready=false;assert(!control::native_apps::openPrepared());
}
''')
 out=p/'test';subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-DORIGAMI_NATIVE_APPS',
  '-I'+str(p),'-I'+str(ROOT/'include'),str(ROOT/'src/control/native_apps.cpp'),str(p/'main.cpp'),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
 print('PASS native lifecycle: original cleanup, callback-safe deferred collection, navigation failure and unavailable module')
