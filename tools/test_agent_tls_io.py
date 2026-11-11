"""Compile the production TLS I/O wrappers: preserve returns/buffers, log fatal Agent errors only."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,content):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
    write('esp_arduino_version.h', '#define ESP_ARDUINO_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_ARDUINO_VERSION 20017\n')
    write('mbedtls/ssl.h', '''#include <cstddef>
struct mbedtls_ssl_context {};
#define MBEDTLS_ERR_SSL_WANT_READ -26880
#define MBEDTLS_ERR_SSL_WANT_WRITE -26752
''')
    write('core/telemetry.h', '''#pragma once
void record(const char*,const char*,const char*,int);
#define LOGW(tag,fmt,op,result) record(tag,fmt,op,result)
''')
    write('main.cpp',r'''
#include <mbedtls/ssl.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
bool worker=false;int result=0,reads=0,writes=0,logs=0;std::string operation;
mbedtls_ssl_context context;unsigned char buffer[4]={1,2,3,4};
namespace services {namespace agent_coordination {bool isCurrentWorker(){return worker;}}}
void record(const char* tag,const char* fmt,const char* op,int code){
 assert(worker&&strcmp(tag,"agent-tls")==0&&strcmp(fmt,"op=%s code=%d")==0&&code==result);
 ++logs;operation=op;
}
extern "C" int __real_mbedtls_ssl_read(mbedtls_ssl_context* ssl,unsigned char* data,size_t n){
 assert(ssl==&context&&data==buffer&&n==sizeof(buffer));++reads;data[0]=42;return result;
}
extern "C" int __real_mbedtls_ssl_write(mbedtls_ssl_context* ssl,const unsigned char* data,size_t n){
 assert(ssl==&context&&data==buffer&&n==sizeof(buffer));++writes;return result;
}
extern "C" int __wrap_mbedtls_ssl_read(mbedtls_ssl_context*,unsigned char*,size_t);
extern "C" int __wrap_mbedtls_ssl_write(mbedtls_ssl_context*,const unsigned char*,size_t);
int main(){
 for(bool owned:{false,true})for(int value:{0,1,4,MBEDTLS_ERR_SSL_WANT_READ,MBEDTLS_ERR_SSL_WANT_WRITE,-1,-80,-30848}){
  worker=owned;result=value;int before=logs;
  assert(__wrap_mbedtls_ssl_read(&context,buffer,sizeof(buffer))==value&&buffer[0]==42);
  assert(__wrap_mbedtls_ssl_write(&context,buffer,sizeof(buffer))==value);
  const bool expected=owned&&value<0&&value!=MBEDTLS_ERR_SSL_WANT_READ&&value!=MBEDTLS_ERR_SSL_WANT_WRITE;
  assert(logs-before==(expected?2:0));if(expected)assert(operation=="write");
 }
 assert(reads==16&&writes==16);
 puts("PASS TLS read/write returns, buffer identity, retry silence and worker-only fatal diagnostics");
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),
                    str(ROOT/'src/services/agent_tls_io.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
