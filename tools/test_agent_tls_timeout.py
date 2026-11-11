"""Exercise the production SDK connection wrapper's Agent-only handshake budget."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def write(name, content):
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    write('Arduino.h', '#pragma once\n#include <cstdint>\n#include <cstddef>\n')
    write('WiFiClientSecure.h', '#pragma once\n#include <cstdint>\nstruct IPAddress {};\nstruct sslclient_context { uint32_t handshake_timeout; };\n')
    write('esp_heap_caps.h', '')
    write('esp_arduino_version.h', '#define ESP_ARDUINO_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_ARDUINO_VERSION 20017\n')
    write('freertos/task.h', '')
    write('lwip/sockets.h', '')
    write('mbedtls/net_sockets.h', '')
    signature = 'sslclient_context* c, const IPAddress& ip, uint32_t port, const char* host, int timeout, const char* ca, bool bundle, const char* cert, const char* key, const char* identity, const char* psk, bool insecure, const char** alpn'
    symbol = '_Z16start_ssl_clientP17sslclient_contextRK9IPAddressjPKciS5_bS5_S5_S5_S5_bPS5_'
    write('main.cpp', '''
#include <WiFiClientSecure.h>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
bool agent=false; unsigned expected=0,calls=0; IPAddress address;
const char caText[]="ca", certText[]="cert", keyText[]="key", hostText[]="host";
const char identityText[]="identity", pskText[]="psk"; const char* protocols[]={"h2",nullptr};
namespace services { namespace agent_coordination { bool isCurrentWorker(){return agent;} } }
namespace core { namespace tls { void rememberSocket(sslclient_context*) {} } }
extern "C" int __real_''' + symbol + '(' + signature + ''') {
 ++calls; assert(c->handshake_timeout==expected && &ip==&address && port==443 && host==hostText);
 assert(timeout==2500 && ca==caText && bundle && cert==certText && key==keyText);
 assert(identity==identityText && psk==pskText && !insecure && alpn==protocols);
 return -123;
}
extern "C" int __wrap_''' + symbol + '(' + signature + ''');
extern "C" int __real_mbedtls_net_send(void*,const unsigned char*,size_t){return 17;}
extern "C" int __real_mbedtls_net_recv(void*,unsigned char*,size_t){return -18;}
int main(){
 for(bool owned:{false,true})for(unsigned limit:{0u,1000u,8000u,120000u}) {
  agent=owned; sslclient_context c{limit}; expected=owned && limit>8000 ? 8000:limit;
  assert(__wrap_''' + symbol + '''(&c,address,443,hostText,2500,caText,true,certText,keyText,identityText,pskText,false,protocols)==-123);
  assert(c.handshake_timeout==limit);
 }
 assert(calls==8);
 puts("PASS Agent handshake deadline, shorter caller deadlines, other-task isolation and original TLS arguments/results");
}
''')
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined', '-I'+str(root), '-I'+str(ROOT/'include'),
                    str(ROOT/'src/services/tls_connection_trace.cpp'), str(root/'main.cpp'), '-o', str(root/'test')], check=True)
    subprocess.run([str(root/'test')], check=True)
