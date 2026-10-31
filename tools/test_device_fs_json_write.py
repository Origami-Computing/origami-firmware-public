"""Exercise the production HTTP JSON socket adapter with slow/partial writes."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/services/device_fs.cpp').read_text()
start=s.index('template<class Transport>\nclass JsonClient')
body=s[start:s.index('// Operation batches',start)]
stub=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
constexpr size_t kChunk=1024;
uint32_t clockMs=0,lastFeed=0,delayMs=0;
unsigned calls=0,stopAt=0,zeroAt=0;
size_t partial=1024;
bool stopping=false;
std::vector<uint8_t> output;
uint32_t millis(){return clockMs;}
namespace core {namespace watchdog {void feed(){assert(uint32_t(clockMs-lastFeed)<15000);lastFeed=clockMs;}}}
namespace services {namespace net_poll {bool stopRequested(){return stopping;}}}
struct Socket {
 virtual size_t write(const uint8_t* data,size_t n){
  assert(n<=1024);++calls;clockMs+=delayMs;
  assert(uint32_t(clockMs-lastFeed)<15000);
  if(calls==stopAt)stopping=true;
  if(calls==zeroAt)return 0;
  n=std::min(n,partial);output.insert(output.end(),data,data+n);return n;
 }
 virtual ~Socket()=default;
};
void reset(){clockMs=lastFeed=0;delayMs=calls=stopAt=zeroAt=0;partial=1024;stopping=false;output.clear();}
'''
checks=r'''
int main(){
 std::vector<uint8_t> payload(5713);for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i*37);
 reset();{JsonClient<Socket> client;Socket& base=client;delayMs=2000;assert(base.write(payload.data(),payload.size())==payload.size());assert(output==payload&&calls==6);}
 reset();{JsonClient<Socket> client;partial=17;assert(client.write(payload.data(),payload.size())==payload.size());assert(output==payload);}
 reset();{JsonClient<Socket> client;stopAt=2;assert(client.write(payload.data(),payload.size())==2048);assert(output.size()==2048&&calls==2);}
 reset();{JsonClient<Socket> client;zeroAt=2;assert(client.write(payload.data(),payload.size())==1024);assert(output.size()==1024);}
 reset();{JsonClient<Socket> client;stopping=true;assert(!client.write(payload.data(),payload.size())&&calls==0);}
 reset();{JsonClient<Socket> client;delayMs=8000;assert(client.write(payload.data(),payload.size())==4096);assert(clockMs==32000&&calls==4);assert(!client.write(payload.data(),1));}
 reset();{JsonClient<Socket> client;clockMs=lastFeed=0xfffffff0;delayMs=2000;assert(client.write(payload.data(),payload.size())==payload.size());assert(output==payload);}
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(stub+body+checks)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual JSON socket adapter: byte-exact 5713-byte body, virtual dispatch, partial writes, stop/failure, watchdog cadence, cooperative deadline and clock wrap')
