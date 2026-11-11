"""Real queue tick: avoid spending HTTPS retry turns before the boot clock is valid."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/upload_queue.cpp').read_text();start=s.index('void UploadQueue::tick() {');end=s.index('\n}',start)+2
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp)
 (p/'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
bool synced=false;const char* backend="https://api.kami.so";
namespace core {namespace time {bool isSynced(){return synced;}} namespace secrets {struct Config {const char* url;};Config backendConfig(){return {backend};}}}
constexpr int WL_CONNECTED=1;struct Wifi {int state=1;int status(){return state;}} WiFi;
struct UploadQueue {bool m_initialized=true,m_uploadPaused=false,running=false,runnable=true;int started=0,notified=0;bool workerRunning(){return running;}bool hasRunnableItem(){return runnable;}void startWorker(){++started;}void notifyWorker(){++notified;}void tick();};
''' + s[start:end] + r'''
int main(){
 UploadQueue q;q.tick();assert(!q.started);synced=true;q.tick();assert(q.started==1);
 synced=false;backend="http://127.0.0.1:8080";q.tick();assert(q.started==2);
 backend="https://api.kami.so";q.running=true;q.tick();assert(q.notified==1&&q.started==2);
 q.m_uploadPaused=true;q.tick();assert(q.notified==1);q.running=false;synced=true;q.tick();assert(q.started==2);
 q.m_uploadPaused=false;WiFi.state=0;q.tick();assert(q.started==2);WiFi.state=1;
 q.runnable=false;q.tick();assert(q.started==2);q.runnable=true;q.m_initialized=false;q.tick();assert(q.started==2);
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS real queue clock admission: HTTPS waits without a retry, plain HTTP works, running/paused/offline/empty/uninitialized behavior preserved')
