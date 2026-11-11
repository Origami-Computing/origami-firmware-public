"""Execute the production SDK adapter with Agent/non-Agent and shorter deadlines."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/services/tls_connection_trace.cpp').read_text()
a=s.index('extern "C" int __real__Z16start_ssl_client')
b=s.index('\nextern "C" int __real_mbedtls_net_send',a)
wrapper=s[a:b]
# Include only the actual connection adapter, excluding independent wire tracing.
code=r'''
#include <cstdint>
#include <cassert>
#include <initializer_list>
#define ORIGAMI_TLS_TRACE 0
struct sslclient_context {unsigned long handshake_timeout;};
namespace core {namespace tls {void rememberSocket(sslclient_context*){}}}
struct IPAddress {};
bool agent;unsigned long expected;int reply,calls;
namespace services {namespace agent_coordination {bool isCurrentWorker(){return agent;}}}
'''+wrapper+r'''
extern "C" int __real__Z16start_ssl_clientP17sslclient_contextRK9IPAddressjPKciS5_bS5_S5_S5_S5_bPS5_(
 sslclient_context* c,const IPAddress&,uint32_t port,const char* host,int timeout,const char* ca,
 bool bundle,const char* cert,const char* key,const char* identity,const char* psk,bool insecure,const char** alpn){
 assert(c->handshake_timeout==expected);assert(port==443&&timeout==4000&&bundle&&!insecure);
 assert(host&&ca&&!cert&&!key&&!identity&&!psk&&!alpn);++calls;return reply;
}
int main(){
 for(bool owner:{false,true})for(unsigned long deadline:{1000ul,8000ul,120000ul})for(int result:{-1,0,7}){
  agent=owner;expected=owner&&deadline>8000?8000:deadline;reply=result;
  sslclient_context c{deadline};IPAddress ip;
  assert(__wrap__Z16start_ssl_clientP17sslclient_contextRK9IPAddressjPKciS5_bS5_S5_S5_S5_bPS5_(
   &c,ip,443,"host",4000,"ca",true,nullptr,nullptr,nullptr,nullptr,false,nullptr)==result);
  assert(c.handshake_timeout==deadline);
 }
 assert(calls==18);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS Agent TLS deadline: owner-only cap, shorter limits, original restoration, security flags and SDK outcomes')
