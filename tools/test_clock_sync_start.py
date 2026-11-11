"""Exercise production SNTP service admission without sockets or clock changes."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/core/board_time.cpp').read_text();a=s.index('void service(');b=s.index('\nvoid prepareForDeepSleep()',a)
code='''#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
bool connected=false; int calls=0;
std::atomic<bool> s_sntp_started{false};bool s_tz_configured=false;
namespace hal { namespace wifi { bool connected(){return ::connected;} } }
static void reportSyncSteps(){}
struct timeval;static void onSntpSync(struct timeval*){}
static const char* kTimezone="PST8PDT";
static bool callbackSet=false;
void sntp_set_time_sync_notification_cb(void(*cb)(struct timeval*)){callbackSet=cb==onSntpSync;}
void configTzTime(const char*,const char*,const char*){++calls;}
'''+s[a:b]+'''
int main(){
 service(0);service(100);assert(calls==0&&!s_sntp_started);
 // The timezone is applied at init(); service() only starts SNTP and its sync callback.
 connected=true;service(200);assert(calls==1&&callbackSet);
 for(unsigned n=201;n<30000;++n)service(n);
 assert(calls==1);connected=false;service(30000);connected=true;service(30001);assert(calls==1);
 puts("PASS SNTP starts on first connected tick and never restarts on reconnect");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11',str(p/'test.cpp'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
