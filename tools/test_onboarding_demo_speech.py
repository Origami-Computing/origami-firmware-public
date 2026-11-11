"""Exercise the production demo speech handler and shared reveal timeline."""
from pathlib import Path
import os, shlex, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/apps/tutorial_app.cpp').read_text()
handler = source[source.index('void serviceSpeech(unsigned ms)'):source.index('\nvoid save()')]
code = r'''
#include "apps/onboarding_demo.h"
#include <cassert>
#include <cstring>
namespace onboarding=apps::onboarding;
struct {unsigned page=124;} s_flow;
bool s_frozen=false,s_speechOpen=false;
unsigned s_speechLetters=0,s_speechBlips=0;
namespace sfx {
unsigned calls=0;bool open=false;
bool typewriterServiceChar(char c,unsigned){if(c<=' ')return false;++calls;open=true;return true;}
bool typewriterIsOpen(){return open;}
}
void stopSpeech(){sfx::open=s_speechOpen=false;}
''' + handler + r'''
int main(){
 for(unsigned t=0;t<2100;t+=20)serviceSpeech(t);
 assert(sfx::calls>20);unsigned question=sfx::calls;
 serviceSpeech(2300);assert(!sfx::open&&sfx::calls==question);
 serviceSpeech(3100);assert(sfx::calls==question);
 for(unsigned t=3120;t<5000;t+=20)serviceSpeech(t);
 assert(sfx::calls>question+40&&!sfx::open);
 unsigned done=sfx::calls;serviceSpeech(10500);assert(sfx::calls==done);
 s_frozen=true;serviceSpeech(1000);assert(sfx::calls==done&&!sfx::open);
 s_frozen=false;s_flow.page=0;serviceSpeech(2000);assert(sfx::calls==done&&!sfx::open);
 const auto end=onboarding::DemoSpeech::at(10000);assert(end.count==70);
}
'''
with tempfile.TemporaryDirectory(dir=ROOT/'.cache') as d:
    p=Path(d);(p/'test.cpp').write_text(code);exe=p/'test.exe'
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++14','-DORIGAMI_DEV_SERIAL_INPUT','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS question/reply blips; silent thinking, end, frozen frames and exit')
