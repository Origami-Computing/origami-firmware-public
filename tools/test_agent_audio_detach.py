"""Race the actual transport ring functions against owner detach/free under ASan."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/services/agent_transport.cpp').read_text()
def function(signature):
    start=source.index(signature+' {');opening=source.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    code=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
bool s_listening=false,s_serverReady=false;
#include <mutex>
#include <thread>
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
#define LOGD(...) ((void)0)
#define LOGI(...) ((void)0)
std::mutex s_audioMux;
bool s_audioAttached=false,s_streamOverflow=false;
uint32_t s_ringRead=0,s_ringWrite=0,s_ringUsed=0,s_streamDropped=0,s_sentSamples=0;
int16_t* buffer=nullptr;
struct Replies {uint32_t reset(){return 1;}}s_replies;
uint32_t s_turnEpoch=0;
bool s_uiCancelled=false,s_uiRejected=false,s_replyReceived=false,s_sendDone=false,
 s_replyReady=false,s_sendCancel=false,s_startSent=false,s_commitSent=false,
 s_commitRequested=false,s_transportLost=false;
uint32_t s_uiCancelTag=0,s_sendCancelNav=0,s_commitAtMs=0;
int s_sendResult=0;
namespace services {namespace agent_coordination {
 void setSessionActive(bool active){assert(active&&s_audioAttached);}
 bool isSessionActive(){return true;}
}}
std::atomic<bool> holdLookup{false},lookupEntered{false};
int lookups=0;
int16_t* pcm(){++lookups;lookupEntered=true;while(holdLookup.load())std::this_thread::yield();return buffer;}
uint32_t capacity(){++lookups;return buffer?8:0;}
'''
    for sig in ['bool streamMicSamples(const int16_t* samples, uint32_t count, void*)',
                'uint32_t peekAudioFrame(uint32_t epoch, int16_t* out, uint32_t maxSamples)',
                'bool dropAudioFrame(uint32_t epoch, uint32_t count)', 'bool rewindRingForResendLocked()',
                'void resetAudioRingLocked()', 'void detachAudio()', 'void beginStreamingRecording()']:
        code+=function(sig)+'\n'
    code+=r'''
bool rewindRingForResend(){std::lock_guard<std::mutex> lock(s_audioMux);return rewindRingForResendLocked();}
int main(){
for(int cycle=0;cycle<40;++cycle){
 buffer=new int16_t[8];beginStreamingRecording();
 int16_t input[]={1,2,3,4},out[4]={};
 assert(streamMicSamples(input,4,nullptr));
 assert(peekAudioFrame(s_turnEpoch,out,4)==4&&out[3]==4);
 dropAudioFrame(s_turnEpoch,2);assert(s_ringUsed==2);
 assert(rewindRingForResend()&&s_ringUsed==4);
 // Pause inside pointer lookup: detach must wait for this borrow/copy to finish.
 holdLookup=true;lookupEntered=false;
 std::thread reader([&]{assert(peekAudioFrame(s_turnEpoch,out,4)==4);});
 while(!lookupEntered.load())std::this_thread::yield();
 std::atomic<bool> detachStarted{false},detached{false};
 std::thread owner([&]{detachStarted=true;detachAudio();detached=true;});
 while(!detachStarted.load())std::this_thread::yield();
 std::this_thread::sleep_for(std::chrono::milliseconds(2));assert(!detached.load());
 holdLookup=false;reader.join();owner.join();
 delete[] buffer;buffer=nullptr;
 int before=lookups;
 assert(peekAudioFrame(s_turnEpoch,out,4)==0);
 assert(!streamMicSamples(input,4,nullptr));
 dropAudioFrame(s_turnEpoch,4);assert(!rewindRingForResend());detachAudio();
 assert(lookups==before&&s_ringUsed==0); // No callbacks touch freed/replaced storage.
}
}
'''
    (root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=address,undefined',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS actual ring: detach waits for in-flight pointer lookup/copy, post-free operations never access storage, 40 detach/rebind cycles under sanitizers')
