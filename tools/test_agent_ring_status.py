"""Race actual ring writers with status() under ThreadSanitizer.

Only ring overflow/drop observation is in scope. Other transport fields remain
constant in this harness; this is not a proof of whole-transport thread safety.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-control', action='store_true')
args = parser.parse_args()
source = (ROOT/'src/services/agent_transport.cpp').read_text()
def function(signature):
    a = source.index(signature+' {'); b = source.index('{', a)+1; depth = 1
    while depth:
        depth += (source[b]=='{')-(source[b]=='}'); b += 1
    return source[a:b]
# Frozen pre-fix status from c069f842; retain it as a race-detection control.
old_status = '''Status status() {
    return {socketReadyForAudio(millis()), s_sendDone, s_commitRequested,
            s_commitSent, s_replyReady, s_streamOverflow, s_streamDropped};
}'''
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
#define LOGD(...) ((void)0)
std::mutex s_audioMux;
bool s_audioAttached=true;
volatile bool s_streamOverflow=false;
volatile uint32_t s_ringRead=0,s_ringWrite=0,s_ringUsed=0,s_streamDropped=0;
bool s_sendDone=false,s_commitRequested=false,s_commitSent=false,s_replyReady=false;
int16_t ring[8];
int16_t* pcm(){return ring;}
uint32_t capacity(){return 8;}
uint32_t millis(){return 123;}
bool socketReadyForAudio(uint32_t){return true;}
bool socketReadyForAudioLocked(uint32_t){return true;}
struct Status {bool ready,done,commitPending,commitSent,replyReady,overflowed;uint32_t droppedSamples;};
'''
for signature in ['void resetAudioRingLocked()', 'bool streamMicSamples(const int16_t* samples, uint32_t count, void*)']:
    code += function(signature)+'\n'
code += old_status if args.negative_control else function('Status status()')
code += r'''
void resetAudioRing(){std::lock_guard<std::mutex> lock(s_audioMux);resetAudioRingLocked();}
int main(){
 std::atomic<bool> ready{false};
 std::thread writer([&]{
  int16_t samples[12]={};ready=true;
  for(unsigned i=0;i<100000;++i){resetAudioRing();assert(!streamMicSamples(samples,12,nullptr));}
 });
 while(!ready.load())std::this_thread::yield();
 for(unsigned i=0;i<100000;++i){
  const auto s=status();
  assert(s.ready&&!s.done&&!s.commitPending&&!s.commitSent&&!s.replyReady);
  assert(s.droppedSamples==0||s.droppedSamples==4);
  assert(s.overflowed==(s.droppedSamples==4));
 }
 writer.join();
 puts("PASS 100000 concurrent resets/overflows and coherent status reads");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory); (p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=thread','-g',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    result=subprocess.run([str(p/'test')],capture_output=True,text=True)
    if args.negative_control:
        assert result.returncode and 'ThreadSanitizer: data race' in result.stderr, result.stderr
        print('PASS negative control: frozen unlocked status produces a ThreadSanitizer data race')
    else:
        if result.returncode: raise RuntimeError(result.stderr)
        print(result.stdout.strip())
