"""Exercise production audio pump/ring functions with a slow, faultable socket.

No hardware or backend calls. Negative control runs the shipped staging.206
functions and must reproduce unbounded draining and retry on a damaged stream.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--negative-control', action='store_true')
args = p.parse_args()
source = (ROOT/'src/services/agent_transport.cpp').read_text()
if args.negative_control:
    source = subprocess.check_output(['git', 'show', 'v0.1.209-staging.206:src/services/agent_transport.cpp'], cwd=ROOT, text=True)

def function(signature):
    a = source.index(signature+' {'); b = source.index('{', a)+1; depth = 1
    while depth:
        depth += (source[b]=='{')-(source[b]=='}'); b += 1
    return source[a:b]+'\n'

# Exercise the pinned library's actual heartbeat timeout with the production
# configuration, rather than substituting a model of its timeout behavior.
heartbeat = re.search(r'enableHeartbeat\((\d+), (\d+), (\d+)\)', source)
if not args.negative_control:
    assert heartbeat, 'Missing production heartbeat configuration'
    interval, timeout, misses = map(int, heartbeat.groups())
    assert interval == 8000 and timeout == 3000 and 1 <= misses <= 3
    library = ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/WebSockets/src/WebSockets.cpp'
    library_source = library.read_text()
    saved_source = source
    source = library_source
    heartbeat_impl = function('void WebSockets::handleHBTimeout(WSclient_t * client)')
    source = saved_source

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include "services/agent_transport_config.h"
#include "core/memory.h"
namespace core { namespace memory {
 Snapshot available{0,0,65536,32768};
 Snapshot snapshot(){return available;}
}}
constexpr unsigned MALLOC_CAP_DMA=1;
size_t dmaLargest=32768;
size_t heap_caps_get_largest_free_block(unsigned caps){assert(caps==MALLOC_CAP_DMA);return dmaLargest;}
unsigned criticalDepth=0;
std::mutex s_audioMux;
#define portENTER_CRITICAL(m) do{(m)->lock();++criticalDepth;}while(0)
#define portEXIT_CRITICAL(m) do{--criticalDepth;(m)->unlock();}while(0)
#define LOGD(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
bool s_audioAttached=true,s_streamOverflow=false,s_socketConnected=true,s_serverReady=true,
 s_listening=false,s_sendCancel=false,s_startSent=false,s_commitSent=false,
 s_commitRequested=false,s_transportLost=false,s_replyReceived=false;
uint32_t s_ringRead=0,s_ringWrite=0,s_ringUsed=0,s_streamDropped=0,s_sentSamples=0,
 s_turnEpoch=1,s_commitAtMs=0,s_wsConnectedSinceMs=0,s_startAtMs=0;
uint32_t clockMs=1000;
uint32_t millis(){return clockMs;}
constexpr uint32_t frameSamples=cfg::agent::kAudioFrameBytes/2;
constexpr uint32_t cap=64*512;
int16_t ring[cap],s_wsFrame[cfg::agent::kAudioFrameBytes/2];
int16_t* pcm(){return ring;}
uint32_t capacity(){return cap;}
namespace services {namespace agent_coordination {
 bool active=true,paused=false;
 void setSessionActive(bool b){active=b;}
 bool isSessionActive(){return active;}
 bool suspendRequested(){return paused;}
}}
enum class SendOutcome {NetworkError};
void resetSessionFlags();
void handleSessionDrop();
unsigned failures=0;
void finishSession(SendOutcome,const char*,uint32_t epoch){
 assert(!criticalDepth);
 if(epoch==s_turnEpoch){++failures;resetSessionFlags();}
}
uint32_t failAudioAt=0,failText=0,cancelAt=0,pauseAt=0,refillUntil=0;
uint32_t writes=0,starts=0,commits=0,disconnects=0;
bool damaged=false;
bool pressureAfterWrite=false;
std::vector<int16_t> received;
bool streamMicSamples(const int16_t*,uint32_t,void*);
struct Socket {
 bool sendTXT(const char* text){
  assert(!criticalDepth);
  const bool start=strstr(text,"start");
  if(start)++starts;else ++commits;
  if(failText==(start?1u:2u)){failText=0;damaged=true;return false;}
  return true;
 }
 bool sendBIN(const uint8_t* data,uint32_t bytes){
  assert(!criticalDepth);assert(bytes<=cfg::agent::kAudioFrameBytes);++writes;
  if(writes==cancelAt)s_sendCancel=true;
  if(writes==pauseAt)services::agent_coordination::paused=true;
  if(writes==failAudioAt){damaged=true;return false;} // Header/prefix may be on wire.
  const auto* samples=reinterpret_cast<const int16_t*>(data);
  received.insert(received.end(),samples,samples+bytes/2);
  if(pressureAfterWrite)core::memory::available.dramFree=8192;
  if(writes<refillUntil){int16_t incoming[frameSamples]={};assert(streamMicSamples(incoming,frameSamples,nullptr));}
  return true;
 }
 void disconnect(){assert(!criticalDepth);++disconnects;damaged=false;received.clear();handleSessionDrop();}
}s_socket;
'''
for signature in ['void resetSessionFlags()', 'bool socketReadyForAudioLocked(uint32_t nowMs)',
                  'void resetAudioRingLocked()', 'bool streamMicSamples(const int16_t* samples, uint32_t count, void*)',
                  'uint32_t peekAudioFrame(uint32_t epoch, int16_t* out, uint32_t maxSamples)',
                  'bool dropAudioFrame(uint32_t epoch, uint32_t count)', 'bool rewindRingForResendLocked()',
                  'void handleSessionDrop()']:
    code += function(signature)
if not args.negative_control:
    code += function('void disconnectAfterWriteFailure(const char* frame)')
    code += function('void expireUnreadySocket(uint32_t nowMs)')
code += function('void sendStartFrame(uint32_t epoch)')+function('void pumpAudio()')
if not args.negative_control:
    code += function('bool sendTextRequest(const char* wire)')
code += r'''
void reset(uint32_t samples){
 resetSessionFlags();resetAudioRingLocked();services::agent_coordination::active=true;
 services::agent_coordination::paused=false;s_socketConnected=s_serverReady=true;s_audioAttached=true;
 s_listening=false;s_replyReceived=false;s_wsConnectedSinceMs=0;clockMs=1000;
 writes=starts=commits=disconnects=failures=0;failAudioAt=failText=cancelAt=pauseAt=refillUntil=0;
 damaged=false;received.clear();
 pressureAfterWrite=false;core::memory::available={0,0,65536,32768};dmaLargest=32768;
 for(uint32_t i=0;i<samples;++i)ring[i]=i;
 s_ringWrite=s_ringUsed=samples;
}
void reconnect(){s_socketConnected=s_serverReady=true;s_startSent=s_listening=false;}
void acknowledge(){assert(s_startSent);s_listening=true;}
'''
if args.negative_control:
    code += r'''
int main(){
 reset(frameSamples);s_startSent=s_listening=true;refillUntil=40;pumpAudio();
 assert(writes==40);printf("REPRODUCED: one pump sent %u frames while capture kept refilling\n",writes);
 reset(1024);s_startSent=s_listening=true;failAudioAt=1;pumpAudio();
 assert(damaged&&disconnects==0&&s_ringUsed==1024);pumpAudio();
 assert(damaged&&writes>1);puts("REPRODUCED: retried frame on a stream containing a partial frame");
}
'''
else:
    code += r'''
int main(){
 // Pressure preserves queued PCM and commit; retry drains the exact take.
 reset(2048);s_startSent=s_listening=true;s_commitRequested=true;
 core::memory::available.dramFree=12287;pumpAudio();
 assert(!writes&&!commits&&s_ringUsed==2048);
 core::memory::available.dramFree=12288;dmaLargest=4095;
 pumpAudio();assert(!writes&&!commits);
 dmaLargest=4096;pressureAfterWrite=true;pumpAudio();
 assert(writes==1&&!commits&&s_ringUsed==1536&&!disconnects);
 pressureAfterWrite=false;core::memory::available={0,0,65536,32768};pumpAudio();
 assert(writes==4&&commits==1&&received.size()==2048&&!disconnects);
 for(unsigned i=0;i<received.size();++i)assert(received[i]==int16_t(i));
 puts("PASS low-memory backpressure and exact replay after recovery without reconnect");
 // Upgrade != server authentication != recording accepted. No premature audio/commit.
 reset(frameSamples);s_commitRequested=true;s_serverReady=false;pumpAudio();
 assert(!starts&&!writes&&!commits);s_serverReady=true;pumpAudio();
 assert(starts==1&&!writes&&!commits);pumpAudio();assert(starts==1&&!writes);
 acknowledge();pumpAudio();assert(writes==1&&commits==1&&s_commitSent);
 pumpAudio();assert(commits==1);puts("PASS ready/listening gates and one commit");
 // A sustained producer cannot trap the worker inside one pump.
 reset(frameSamples);s_startSent=s_listening=true;refillUntil=40;pumpAudio();
 assert(writes==cfg::agent::kAudioFramesPerPump&&s_ringUsed==frameSamples);
 puts("PASS bounded draining with concurrent capture");
 // Partial audio frame: close, rewind ALL acknowledged sends, start/ack then replay.
 reset(3072);s_startSent=s_listening=true;s_commitRequested=true;failAudioAt=3;pumpAudio();
 assert(disconnects==1&&!damaged&&!s_socketConnected&&!s_serverReady&&!s_listening);
 assert(s_ringUsed==3072&&s_sentSamples==0&&!commits);reconnect();pumpAudio();
 assert(starts==1&&writes==3);acknowledge();for(unsigned i=0;i<16&&!s_commitSent;++i)pumpAudio();
 assert(s_commitSent&&commits==1&&received.size()==3072);
 for(unsigned i=0;i<received.size();++i)assert(received[i]==int16_t(i));
 puts("PASS failed write closes stream and replays exact take on new connection");
 // Even a failed start can have a prefix on wire.
 reset(frameSamples);failText=1;pumpAudio();assert(disconnects==1&&!writes&&!s_startSent);
 reset(0);failText=2;assert(!sendTextRequest("text"));assert(disconnects==1&&!damaged);
 // Failed commit is ambiguous: never execute the user's query a second time.
 reset(frameSamples);s_startSent=s_listening=true;s_commitRequested=true;failText=2;pumpAudio();
 assert(failures==1&&disconnects==1&&!services::agent_coordination::active);
 reconnect();pumpAudio();assert(commits==1);
 puts("PASS start failure recovery and no replay after ambiguous commit");
 for(bool pause:{false,true}){
  reset(4096);s_startSent=s_listening=true;s_commitRequested=true;
  if(pause)pauseAt=1;else cancelAt=1;pumpAudio();
  assert(writes==1&&!commits&&s_ringUsed==4096-frameSamples);
 }
 puts("PASS cancellation and resource pause checked between writes");
 // Failed stream loses recording acknowledgment and cannot reuse it on reconnect.
 reset(frameSamples);s_startSent=s_listening=true;handleSessionDrop();reconnect();pumpAudio();
 assert(starts==1&&!writes&&!s_listening);
 // Upgraded-but-never-ready sockets recover without sending unauthenticated audio.
 reset(frameSamples);s_serverReady=false;s_wsConnectedSinceMs=100;
 expireUnreadySocket(100+cfg::agent::kReadyTimeoutMs-1);assert(!disconnects);
 expireUnreadySocket(100+cfg::agent::kReadyTimeoutMs);assert(disconnects==1&&!s_socketConnected);
 assert(s_ringUsed==frameSamples&&!s_sentSamples&&!s_transportLost);
 reset(frameSamples);expireUnreadySocket(100000);assert(!disconnects); // Authenticated idle connection.
 reset(frameSamples);pumpAudio();assert(s_startSent && s_startAtMs==clockMs);
 expireUnreadySocket(clockMs+cfg::agent::kReadyTimeoutMs-1);assert(!disconnects);
 expireUnreadySocket(clockMs+cfg::agent::kReadyTimeoutMs);assert(disconnects==1&&s_ringUsed==frameSamples);
 reset(frameSamples);pumpAudio();acknowledge();expireUnreadySocket(100000);assert(!disconnects);
 reset(frameSamples);pumpAudio();s_commitSent=true;expireUnreadySocket(100000);assert(!disconnects);
 reset(frameSamples);clockMs=UINT32_MAX-100;pumpAudio();
 expireUnreadySocket(200);assert(!disconnects);
 expireUnreadySocket(cfg::agent::kReadyTimeoutMs);assert(disconnects==1);
 reset(frameSamples);s_serverReady=false;s_wsConnectedSinceMs=UINT32_MAX-100;
 expireUnreadySocket(200);assert(!disconnects);
 expireUnreadySocket(cfg::agent::kReadyTimeoutMs);assert(disconnects==1);
 puts("PASS missing-ready deadline, retained take, authenticated isolation and clock wrap");
 // Stable-link comparison remains valid over millis wrap.
 reset(0);s_wsConnectedSinceMs=UINT32_MAX-100;
 assert(!socketReadyForAudioLocked(200));assert(socketReadyForAudioLocked(500));
 puts("PASS reconnect acknowledgment reset and clock wrap");
}
'''
if not args.negative_control:
    code = code.replace('int main(){', 'int audioChecks(){')
    code = code.replace('puts("PASS reconnect acknowledgment reset and clock wrap");',
                        'puts("PASS reconnect acknowledgment reset and clock wrap");return 0;')
    code += r'''
#define DEBUG_WEBSOCKETS(...) ((void)0)
struct WSclient_t {
 uint32_t pingInterval,lastPing,pongTimeout;
 unsigned pongTimeoutCount,disconnectTimeoutCount;
 bool pongReceived;
};
struct WebSockets {
 unsigned closed=0;
 void clientDisconnect(WSclient_t*){++closed;}
 void handleHBTimeout(WSclient_t*);
};
''' + heartbeat_impl + f'''
int main(){{
 audioChecks();WebSockets socket;
 WSclient_t c{{{interval},100,{timeout},0,{misses},false}};
 clockMs=100+{timeout};socket.handleHBTimeout(&c);assert(!socket.closed);
 for(unsigned i=0;i<{misses};++i){{
  c.lastPing=clockMs;clockMs+={timeout}+1;socket.handleHBTimeout(&c);
  assert(socket.closed==(i+1=={misses}));
 }}
 c.pongReceived=true;socket.handleHBTimeout(&c);assert(c.pongTimeoutCount==0);
 c.pongReceived=false;c.lastPing=UINT32_MAX-100;clockMs={timeout}+1;
 socket.handleHBTimeout(&c);assert(c.pongTimeoutCount==1);
 puts("PASS pinned WebSocket heartbeat: bounded stale-stream cleanup, pong recovery, clock wrap");
}}
'''

with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    if args.negative_control:
        (root/'services').mkdir()
        (root/'services/agent_transport_config.h').write_text(subprocess.check_output(
            ['git','show','v0.1.209-staging.206:include/services/agent_transport_config.h'],cwd=ROOT,text=True))
    subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=address,undefined',
                    '-I'+str(root),'-I'+str(ROOT/'include'),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
