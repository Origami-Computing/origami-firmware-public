"""Compile actual configuration request/worker gate; test deferred and racing calls.

Socket cleanup is fake. This checks request ownership, not credential storage,
TLS cleanup, session admission, or the rest of the worker's shared flags.
"""
from pathlib import Path
import argparse,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-control',action='store_true')
args=parser.parse_args()
source=(ROOT/'src/services/agent_transport.cpp').read_text()
def block(marker):
 a=source.index(marker);b=source.index('{',a)+1;n=1
 while n:n+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
gate=block('        if (!services::agent_coordination::isSessionActive() &&')
request=block('void reconfigure()')
declaration=re.search(r'^static .*s_reconfigureRequest[^\n]+',source,re.M)[0]
if args.negative_control:
 # The pre-fix declaration/gate at edd2d177, without requiring Git history.
 declaration='static volatile bool s_reconfigureRequest = false;'
 request='void reconfigure() { s_reconfigureRequest = true; }'
 gate='''if ((s_reconfigureRequest || apiKeyRevision != s_apiKeyRevision) && !services::agent_coordination::isSessionActive()) {
 s_reconfigureRequest=false;s_apiKeyRevision=apiKeyRevision;
 services::agent_coordination::setWorkerPhase(coordination::NetworkCleanup);
 s_socket.disconnect();s_socketConfigured=false;
}'''
code=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <cstdio>
void reconfigure();
bool active=false,injectRequest=false,s_socketConfigured=true;
unsigned cleanups=0,phases=0;
uint32_t revision=0,s_apiKeyRevision=0;
namespace services {namespace agent_coordination {
 enum class WorkerPhase {NetworkCleanup};
 bool isSessionActive(){return active;}
 void setWorkerPhase(WorkerPhase){++phases;}
}}
using coordination=services::agent_coordination::WorkerPhase;
struct Socket {void disconnect(){++cleanups;if(injectRequest){injectRequest=false;reconfigure();}}}s_socket;
'''+declaration+'\n'+request+'\nvoid workerStep(){const uint32_t apiKeyRevision=revision;\n'+gate+'}\n'
code+=r'''
int main(){
 workerStep();assert(cleanups==0);
 active=true;reconfigure();reconfigure();revision=1;workerStep();
 assert(cleanups==0&&s_apiKeyRevision==0&&s_socketConfigured);
 active=false;workerStep();assert(cleanups==1&&s_apiKeyRevision==1&&!s_socketConfigured);
 workerStep();assert(cleanups==1); // Requests before consumption coalesce.
 s_socketConfigured=true;revision=2;workerStep();assert(cleanups==2&&!s_socketConfigured);
 injectRequest=true;reconfigure();workerStep();assert(cleanups==3);
 workerStep();assert(cleanups==4); // Request during cleanup survives for next iteration.
 std::atomic<bool> start{false};
 std::thread producer([&]{while(!start.load())std::this_thread::yield();for(int i=0;i<100000;++i)reconfigure();});
 start=true;for(int i=0;i<100000;++i)workerStep();producer.join();workerStep();
 assert(phases==cleanups);unsigned settled=cleanups;workerStep();assert(cleanups==settled);
 puts("PASS active-turn deferral, coalescing, revision refresh, request during cleanup and 100000 racing requests");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=thread','-g',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 result=subprocess.run([str(p/'test')],capture_output=True,text=True)
 if args.negative_control:
  assert result.returncode and 'ThreadSanitizer: data race' in result.stderr,result.stderr
  print('PASS negative control: volatile request produces a ThreadSanitizer data race')
 else:
  if result.returncode:raise RuntimeError(result.stderr)
  print(result.stdout.strip())
