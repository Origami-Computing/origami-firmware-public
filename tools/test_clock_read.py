"""Exercise the actual clock formatter against Arduino's zero-timeout boundary.
--source-ref origin/staging --expect-glitch reproduces intermittent blank HH:MM.
"""
from pathlib import Path
import argparse,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--source-ref');p.add_argument('--expect-glitch',action='store_true');args=p.parse_args()
src=subprocess.check_output(['git','show',args.source_ref+':src/core/board_time.cpp'],cwd=ROOT,text=True) if args.source_ref else (ROOT/'src/core/board_time.cpp').read_text()
def fn(signature):
 a=src.index(signature);i=src.index('{',a)+1;depth=1
 while depth:depth+=(src[i]=='{')-(src[i]=='}');i+=1
 return src[a:i]
code=r'''
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>
static time_t epoch=1801856040; // Fixed synchronized epoch; expected below uses localtime.
extern "C" time_t time(time_t* p){if(p)*p=epoch;return epoch;}
static uint32_t ticks=1000,step=0;
uint32_t millis(){uint32_t result=ticks;ticks+=step;return result;}
void delay(uint32_t ms){ticks+=ms;}
// Arduino 2.0.17 esp32-hal-time.c getLocalTime implementation. A deadline
// crossing must never hide an already-synchronized clock in our formatter.
bool getLocalTime(struct tm* info,uint32_t ms){
 uint32_t start=millis();time_t now;
 while((millis()-start)<=ms){time(&now);localtime_r(&now,info);if(info->tm_year>(2016-1900))return true;delay(10);}
 return false;
}
static constexpr time_t kSyncedEpochThreshold=1700000000;
static int64_t lastSync=epoch;
static int64_t lastNetworkSyncEpoch(){return lastSync;}
static bool clockDisplayFresh(int64_t now,int64_t sync){return sync>0&&now-sync<=5LL*24*60*60;}
'''+fn('bool isSynced()')+'\n'+fn('void formatClockHHMM(')+r'''
int main(){
 setenv("TZ","UTC0",1);tzset();char out[8],expected[8];tm t{};localtime_r(&epoch,&t);snprintf(expected,sizeof(expected),"%02d:%02d",t.tm_hour,t.tm_min);
 step=0;formatClockHHMM(out,sizeof(out));assert(!strcmp(out,expected));
 step=1;formatClockHHMM(out,sizeof(out));
'''
code+= 'assert(!out[0]);\n' if args.expect_glitch else 'assert(!strcmp(out,expected));\n'
code+=r'''
 step=0;formatClockHHMM(out,sizeof(out));assert(!strcmp(out,expected));
 epoch+=60;localtime_r(&epoch,&t);snprintf(expected,sizeof(expected),"%02d:%02d",t.tm_hour,t.tm_min);formatClockHHMM(out,sizeof(out));assert(!strcmp(out,expected));
 // Hidden once the last network sync is more than five days old.
 lastSync=epoch-5LL*24*60*60-1;formatClockHHMM(out,sizeof(out));assert(!out[0]);
 lastSync=epoch;
 epoch=0;formatClockHHMM(out,sizeof(out));assert(!out[0]);
 formatClockHHMM(nullptr,8);char guard[]="abc";formatClockHHMM(guard,4);assert(!strcmp(guard,"abc"));
 puts("RESULT");}
'''.replace('RESULT','REPRODUCED synchronized clock -> blank -> clock on millisecond boundary' if args.expect_glitch else 'PASS clock survives millisecond boundary, updates minute, stays unavailable before synchronization')
with tempfile.TemporaryDirectory() as d:
 d=Path(d);(d/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++14','-fsanitize=address,undefined',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
