"""Exercise production TLS teardown with controlled socket and TCP endpoints."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 (p/'fake.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cassert>
#define ESP_ARDUINO_VERSION_VAL(a,b,c) 2017
#define ESP_ARDUINO_VERSION 2017
#define CONFIG_LWIP_MAX_SOCKETS 4
struct sslclient_context {int socket;};
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
using err_t=int;constexpr int ERR_OK=0;
struct tcpip_api_call_data {};
struct ip_addr_t {uint32_t addr;bool v4=true;};
#define IP_IS_V4(p) ((p)->v4)
#define ip_2_ip4(p) (p)
#define ip4_addr_get_u32(p) ((p)->addr)
enum {CLOSED,LISTEN,SYN_SENT,SYN_RCVD,ESTABLISHED,FIN_WAIT_1,FIN_WAIT_2,CLOSE_WAIT,CLOSING,LAST_ACK,TIME_WAIT};
struct tcp_pcb {tcp_pcb* next=nullptr;int state=ESTABLISHED;ip_addr_t local_ip{1},remote_ip{2};uint16_t local_port=100,remote_port=443;};
extern tcp_pcb* tcp_active_pcbs;
void tcp_abort(tcp_pcb*);
err_t tcpip_api_call(err_t(*)(tcpip_api_call_data*),tcpip_api_call_data*);
using socklen_t=unsigned;constexpr int AF_INET=2;
struct sockaddr {int unused;};
struct sockaddr_in {int sin_family=AF_INET;uint16_t sin_port;struct {uint32_t s_addr;} sin_addr;};
inline uint16_t ntohs(uint16_t x){return x;}
int getsockname(int,sockaddr*,socklen_t*);int getpeername(int,sockaddr*,socklen_t*);
''')
 for name in ['WiFiClientSecure.h','esp_arduino_version.h','freertos/FreeRTOS.h','freertos/portmacro.h','lwip/sockets.h','lwip/tcpip.h','lwip/priv/tcpip_priv.h','lwip/priv/tcp_priv.h']:
  h=p/name;h.parent.mkdir(parents=True,exist_ok=True);h.write_text('#include "fake.h"\n')
 code=r'''
#include "fake.h"
#include "SOURCE"
tcp_pcb* tcp_active_pcbs=nullptr;
int aborts=0,stops=0,callbacks=0;bool failLookup=false,failCallback=false;
int getsockname(int fd,sockaddr* out,socklen_t*){assert(fd>=0);auto* a=(sockaddr_in*)out;a->sin_addr.s_addr=1;a->sin_port=100;return failLookup?-1:0;}
int getpeername(int,sockaddr* out,socklen_t*){auto* a=(sockaddr_in*)out;a->sin_addr.s_addr=2;a->sin_port=443;return 0;}
void tcp_abort(tcp_pcb* p){assert(stops>callbacks);++aborts;p->state=CLOSED;}
err_t tcpip_api_call(err_t(*fn)(tcpip_api_call_data*),tcpip_api_call_data* data){if(failCallback)return -1;auto r=fn(data);++callbacks;return r;}
extern "C" void __real__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(sslclient_context* c,const char*,const char*,const char*){++stops;c->socket=0;}
void stop(sslclient_context& c){__wrap__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(&c,nullptr,nullptr,nullptr);assert(c.socket==-1);}
int main(){
 for(int state=CLOSED;state<=TIME_WAIT;++state)for(int match=0;match<6;++match){
  tcp_pcb pcb;pcb.state=state;tcp_active_pcbs=&pcb;
  sslclient_context c{50};core::tls::rememberSocket(&c);
  if(match==1)pcb.local_port++;if(match==2)pcb.remote_port++;
  if(match==3)pcb.local_ip.addr++;if(match==4)pcb.remote_ip.addr++;
  if(match==5)pcb.remote_ip.v4=false;
  int before=aborts;stop(c);
  bool closing=state==FIN_WAIT_1||state==FIN_WAIT_2||state==CLOSING||state==LAST_ACK;
  assert(aborts-before==int(match==0&&closing));
  before=callbacks;stop(c);assert(callbacks==before);
 }
 tcp_pcb live,closed;live.next=&closed;closed.state=FIN_WAIT_1;tcp_active_pcbs=&live;
 sslclient_context c{50};core::tls::rememberSocket(&c);int before=aborts;stop(c);
 assert(live.state==ESTABLISHED&&closed.state==CLOSED&&aborts==before+1);
 failLookup=true;c.socket=50;core::tls::rememberSocket(&c);before=callbacks;stop(c);assert(callbacks==before);failLookup=false;
 failCallback=true;c.socket=50;closed.state=FIN_WAIT_1;core::tls::rememberSocket(&c);stop(c);assert(closed.state==FIN_WAIT_1);failCallback=false;
 before=callbacks;stop(c);assert(callbacks==before);
 c.socket=-1;core::tls::rememberSocket(&c);stop(c);assert(callbacks==before);
 // Re-registering the same owner updates rather than consumes additional slots.
 for(int i=0;i<20;++i){c.socket=50;core::tls::rememberSocket(&c);}
 before=callbacks;stop(c);assert(callbacks==before+1);
}
'''.replace('SOURCE',str(ROOT/'src/core/tls_socket_cleanup.cpp'))
 (p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS exact closed TLS endpoint, live/TIME_WAIT isolation, cleanup ordering, repeat stop, lookup/callback failure')
