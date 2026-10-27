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
extern uint32_t now;
inline uint32_t millis(){return now++;} inline void delay(int n){now+=n;}
struct String {std::string s; String(){} String(const char*x):s(x){} String& operator=(const char*x){s=x;return *this;}
 size_t length()const{return s.size();} bool reserve(unsigned n){s.reserve(n);return true;}
 bool concat(const char*x,unsigned n){s.append(x,n);return true;}};
''')
 (p/'HTTPClient.h').write_text(r'''
#pragma once
#include "Arduino.h"
constexpr int HTTPC_DISABLE_FOLLOW_REDIRECTS=0;
struct Stream {std::string body;int reads=0;bool stalled=false;
 int available(){return stalled?0:body.size();}
 int read(uint8_t*out,size_t n){++reads;std::memcpy(out,body.data(),n);body.erase(0,n);return n;}};
struct HTTPClient {int length=0;Stream stream;bool alive=true;String encoding;
 void useHTTP10(bool){} void setReuse(bool){} void setTimeout(int){} void setFollowRedirects(int){} void collectHeaders(const char**,int){}
 int getSize(){return length;} String header(const char*){return encoding;}
 Stream* getStreamPtr(){return &stream;} bool connected(){return alive;}};
''')
 (p/'test.cpp').write_text(r'''
#include "services/bounded_http.h"
#include <cassert>
uint32_t now=0;
int main(){using services::bounded_http::read;String out;HTTPClient h;
 h.length=4;h.stream.body="abcd";assert(read(h,4,out)&&out.s=="abcd");
 h.length=5;h.stream.body="12345";h.stream.reads=0;assert(!read(h,4,out)&&h.stream.reads==0&&out.length()==0);
 h.length=-1;assert(!read(h,100,out)&&h.stream.reads==0);
 h.length=4;h.encoding="chunked";assert(!read(h,100,out)&&h.stream.reads==0);
 h.encoding="";h.stream.body="ab";h.alive=false;assert(!read(h,100,out)&&out.length()==0);
 h.alive=true;h.stream.stalled=true;now=0;assert(!read(h,100,out)&&out.length()==0&&now>=10000);
 h.stream.stalled=false;h.stream.body=std::string(1024,'x');h.length=1024;assert(read(h,1024,out)&&out.length()==1024);
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS bounded body: exact limit, oversized, unknown/chunked, truncation, stall and multi-buffer reads')
