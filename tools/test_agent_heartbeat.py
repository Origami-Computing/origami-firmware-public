"""Exercise the pinned WebSockets heartbeat with the production Agent settings.

Requires the N16R8 PlatformIO dependencies to have been installed by a build.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/WebSockets/src/WebSockets.cpp').read_text()
def function(signature):
    a=source.index(signature); b=source.index('{',a)+1; depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
settings=re.search(r's_socket.enableHeartbeat\((\d+), (\d+), (\d+)\)',
                   (ROOT/'src/services/agent_transport.cpp').read_text()).groups()
code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#define DEBUG_WEBSOCKETS(...) ((void)0)
uint32_t now=0;uint32_t millis(){return now;}
struct WSclient_t {uint32_t pingInterval=0,pongTimeout=0,lastPing=0;
 uint8_t disconnectTimeoutCount=0,pongTimeoutCount=0;bool pongReceived=false;};
struct WebSockets {
 unsigned drops=0;
 void enableHeartbeat(WSclient_t*,uint32_t,uint32_t,uint8_t);
 void handleHBTimeout(WSclient_t*);
 void clientDisconnect(WSclient_t*){++drops;}
};
'''
code+=function('void WebSockets::enableHeartbeat(')+'\n'+function('void WebSockets::handleHBTimeout(')
code+='\nint main(){WebSockets ws;WSclient_t c;ws.enableHeartbeat(&c,'+','.join(settings)+');\n'
code+=r'''
 assert(c.disconnectTimeoutCount==2); // A dead connection must not survive 255 misses.
 c.lastPing=now=1000;
 now+=c.pongTimeout;ws.handleHBTimeout(&c);assert(!ws.drops&&!c.pongTimeoutCount);
 ++now;ws.handleHBTimeout(&c);assert(!ws.drops&&c.pongTimeoutCount==1);
 c.lastPing=now;c.pongReceived=true;ws.handleHBTimeout(&c);assert(!c.pongTimeoutCount);
 // Two unanswered pings, with the client's next ping resetting lastPing.
 c.pongReceived=false;now+=c.pongTimeout+1;ws.handleHBTimeout(&c);assert(!ws.drops);
 c.lastPing=now;now+=c.pongTimeout+1;ws.handleHBTimeout(&c);assert(ws.drops==1);
 // Wrap-safe elapsed time, successful pong, then disabled heartbeat.
 ws.drops=0;c.pongTimeoutCount=0;c.lastPing=UINT32_MAX-100;now=200;
 ws.handleHBTimeout(&c);assert(!ws.drops&&!c.pongTimeoutCount);
 now=c.pongTimeout+1;ws.handleHBTimeout(&c);assert(c.pongTimeoutCount==1);
 c.pongReceived=true;ws.handleHBTimeout(&c);assert(!c.pongTimeoutCount);
 c.pingInterval=0;c.pongReceived=false;now+=100000;ws.handleHBTimeout(&c);assert(!ws.drops);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS pinned heartbeat: healthy pong, delayed reply, two missed replies, disabled mode and clock wrap')
