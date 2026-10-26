"""Exercise the actual drain with queue/HTTP faults and detect self-report loops."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-file', type=Path)
args = parser.parse_args()
source = (args.source_file or ROOT / 'src/services/net_poll.cpp').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
using String=std::string;
bool allocationFailure=false, stopped=false, waiting=false;
int allocations=0, posts=0, response=200, localReports=0;
constexpr int TELEMETRY_BATCH_MAX=8, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
constexpr auto TELEMETRY_LOGS_PATH="logs", TELEMETRY_CRASH_PATH="crash", TELEMETRY_HEARTBEAT_PATH="heartbeat";
void* heap_caps_malloc(size_t n,int){if(allocationFailure)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p){--allocations;free(p);}
namespace core {namespace watchdog {void feed(){}}
namespace telemetry_queue {
enum {KIND_LOG,KIND_VERBOSE,KIND_CRASH,KIND_HEARTBEAT,KIND_USAGE};
struct Item {int kind;char json[512];};
std::vector<Item> queue;
void add(int kind){Item i{};i.kind=kind;strcpy(i.json,"{}");queue.push_back(i);}
bool hasPending(){return !queue.empty();}
int snapshot(Item* out,int cap){int n=std::min(cap,(int)queue.size());std::copy_n(queue.begin(),n,out);return n;}
void ack(int n){queue.erase(queue.begin(),queue.begin()+n);}
struct Stats {unsigned long queued,droppedVerbose,droppedDurable;};
Stats stats(){return {queue.size(),0,0};}
}}
namespace background_network {struct PeriodicWork {static bool hasWaitingPeer(){return waiting;}};}
bool stopRequested(){return stopped;}
int postJson(const char*,const String&){++posts;return response;}
String buildLogsBody(const core::telemetry_queue::Item*,int){return "{}";}
// LOGI models the real facade: every call becomes another outbound item.
#define LOGI(...) core::telemetry_queue::add(core::telemetry_queue::KIND_VERBOSE)
struct {template<class... T>void printf(const char*,T...){++localReports;}} Serial;
''' + function('bool ok2xx(') + '\n' + function('bool retryable(') + '\n' + function('void drainTelemetry(') + r'''
int main(){
 using namespace core::telemetry_queue;
 // One external event must drain to idle, with no self-generated traffic.
 add(KIND_VERBOSE);
 for(int i=0;i<100;++i)drainTelemetry();
 assert(queue.empty() && posts==1 && localReports==1 && allocations==0);
 for(int kind:{KIND_LOG,KIND_VERBOSE,KIND_CRASH,KIND_HEARTBEAT,KIND_USAGE}){
  for(int status:{200,201,400,401,404,408,429,500,503,-1}){
   queue.clear();add(kind);response=status;posts=0;
   drainTelemetry();assert(posts==1 && allocations==0);
   assert(queue.size()==((kind==KIND_USAGE ? !ok2xx(status) : retryable(status))?1u:0u));
  }
 }
 for(int fault=0;fault<3;++fault){
  queue.clear();add(KIND_LOG);posts=0;
  allocationFailure=fault==0;stopped=fault==1;waiting=fault==2;
  drainTelemetry();assert(posts==0 && queue.size()==1 && allocations==0);
 }
 allocationFailure=stopped=waiting=false;response=200;
 queue.clear();for(int i=0;i<20;++i)add(i%4);
 for(int i=0;i<10;++i)drainTelemetry();
 assert(queue.empty() && allocations==0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined',
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('PASS drain reaches idle without self-report traffic; HTTP faults, cancellation, allocation cleanup and mixed batches')
