#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 (p/'Arduino.h').write_text(r'''
#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
extern uint32_t now;
inline uint32_t millis(){return now++;} inline void delay(int n){now+=n;}
struct String {std::string s; String(){} String(const char*x):s(x){} String& operator=(const char*x){s=x;return *this;}
 const char* c_str()const{return s.c_str();} size_t length()const{return s.size();} bool reserve(unsigned n){s.reserve(n);return true;}
 bool concat(const char*x,unsigned n){s.append(x,n);return true;}};
''')
 (p/'HTTPClient.h').write_text(r'''
#pragma once
#include "Arduino.h"
constexpr int HTTPC_DISABLE_FOLLOW_REDIRECTS=0;
struct Stream {std::string body;int reads=0;bool stalled=false;
 virtual ~Stream()=default;
 virtual int available(){return stalled?0:body.size();}
 virtual int read(){return -1;} virtual int peek(){return -1;} virtual void flush(){}
 virtual size_t write(uint8_t){return 0;} virtual size_t write(const uint8_t*,size_t){return 0;}
 int read(uint8_t*out,size_t n){++reads;std::memcpy(out,body.data(),n);body.erase(0,n);return n;}};
struct HTTPClient {int length=0;Stream stream;bool alive=true;String encoding;
 void useHTTP10(bool){} void setReuse(bool){} void setTimeout(int){} void setFollowRedirects(int){} void collectHeaders(const char**,int){}
 int getSize(){return length;} String header(const char*){return encoding;}
 Stream* getStreamPtr(){return &stream;} bool connected(){return alive;}};
''')
 source=(root/'src/widgets/sync.cpp').read_text()
 helpers=source[source.index('class PsramBody'):source.index('// Fetch the raw manifest')]
 code=r'''
#include "services/bounded_http.h"
#include <cassert>
uint32_t now=0;
namespace ArduinoJson {struct Allocator {virtual ~Allocator()=default;virtual void* allocate(size_t)=0;virtual void deallocate(void*)=0;virtual void* reallocate(void*,size_t)=0;};}
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
bool noMemory=false,paused=false;int allocations=0;
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);if(noMemory)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p){if(p){--allocations;free(p);}}
bool pollPausePending(){return paused;}
'''+helpers+r'''
template<class Body>void cases(Body& out){using services::bounded_http::read;HTTPClient h;
 h.length=4;h.stream.body="abcd";assert(read(h,4,out)&&std::string(out.c_str())=="abcd");
 h.length=5;h.stream.body="12345";h.stream.reads=0;assert(!read(h,4,out)&&h.stream.reads==0&&out.length()==0);
 h.length=-1;assert(!read(h,100,out)&&h.stream.reads==0);
 h.length=4;h.encoding="chunked";assert(!read(h,100,out)&&h.stream.reads==0);
 h.encoding="";h.stream.body="ab";h.alive=false;assert(!read(h,100,out)&&out.length()==0);
 h.alive=true;h.stream.stalled=true;now=0;assert(!read(h,100,out)&&out.length()==0&&now>=10000);
 h.stream.stalled=false;h.stream.body=std::string(1024,'x');h.length=1024;assert(read(h,1024,out)&&out.length()==1024);
 }
int main(){String plain;cases(plain);
 {PsramBody body(1024);cases(body);}
 assert(allocations==0);
 {noMemory=true;PsramBody body(4);HTTPClient h;h.length=4;h.stream.body="abcd";assert(!services::bounded_http::read(h,4,body));noMemory=false;}
 {PsramBody body(4);HTTPClient h;h.length=4;h.stream.body="abcd";paused=true;assert(!services::bounded_http::read(h,4,body)&&body.size()==0);paused=false;}
 {ManifestMemory memory;void* p=memory.allocate(60000);assert(p&&!memory.allocate(6000));assert(memory.reallocate(p,100)==p);memory.deallocate(p);}
 assert(allocations==0);
}
'''
 (p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS bounded body: exact limit, oversized, unknown/chunked, truncation, stall and multi-buffer reads')
