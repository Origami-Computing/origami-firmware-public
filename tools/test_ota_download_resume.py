#!/usr/bin/env python3
"""Exercise the real download/resume loop with interrupted and hostile responses."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/services/ota_client.cpp').read_text()
body=s[s.index('static AttemptResult resumeDownload('):s.index('static void downloadTask(')]
code=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using String=std::string;using std::min;
constexpr int HTTP_CODE_OK=200,HTTP_CODE_PARTIAL_CONTENT=206,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,ESP_OK=0;
using esp_ota_handle_t=int;using esp_err_t=int;
void* heap_caps_malloc(size_t n,int){return malloc(n);}void heap_caps_free(void*p){free(p);}
unsigned ticks=0;unsigned millis(){return ticks++;}void vTaskDelay(unsigned n){ticks+=n;}
#define pdMS_TO_TICKS(n) (n)
struct {template<class...T>void printf(const char*,T...){} }Serial;
enum class AttemptResult{Success,Retry,Failed};
std::atomic<bool>s_cancel{false};std::atomic<int>s_written{0};
struct {const char*url="https://fixture.invalid/immutable";int size=256;uint8_t sha256[32]={};}s_release;
char s_downloadKey[65]="synthetic-fixture",s_err[96]={};const void*s_candidate=nullptr;
void setError(const char*s){snprintf(s_err,sizeof(s_err),"%s",s);}
std::vector<uint8_t>expected,flashed;
int requests=0,begins=0,ends=0,aborts=0,resumeStatus=206,dropLimit=100;
bool interrupted=false,alwaysDrop=false,badRange=false,badSize=false,corrupt=false,cancelRead=false,missingStream=false;
std::vector<int>rangeOffsets;
struct WiFiClient {
 int pos=0,limit=256,offset=0;
 int available(){return min(17,limit-pos);}bool connected(){return pos<limit;}
 int read(uint8_t*b,size_t n){int count=min((int)n,limit-pos);for(int i=0;i<count;++i)b[i]=expected[offset+pos+i]^(corrupt && offset+pos+i==200?1:0);pos+=count;if(cancelRead)s_cancel=true;return count;}
}stream;
struct WiFiClientSecure{void stop(){stream.limit=stream.pos;}};
struct HTTPClient{
 int offset=0;
 bool begin(WiFiClientSecure&,const char*){offset=0;return true;}
 void end(){}void collectHeaders(const char**,int){}
 void addHeader(const char*k,const String&v){if(!strcmp(k,"Range")){assert(sscanf(v.c_str(),"bytes=%d-",&offset)==1);rangeOffsets.push_back(offset);}}
 int GET(){++requests;stream.offset=offset;stream.pos=0;stream.limit=(alwaysDrop || (interrupted && requests==1))?min(dropLimit,s_release.size-offset):s_release.size-offset;return requests==1?200:resumeStatus;}
 int getSize(){return s_release.size-offset+(badSize && requests>1?1:0);}
 String header(const char*){return "bytes "+std::to_string(offset+(badRange?1:0))+"-255/256";}
 WiFiClient* getStreamPtr(){return missingStream && requests>1?nullptr:&stream;}
};
void configureHttp(HTTPClient&,WiFiClientSecure&){}
int esp_ota_begin(const void*,int,esp_ota_handle_t*h){++begins;flashed.clear();*h=1;return 0;}
int esp_ota_write(int,const uint8_t*b,int n){flashed.insert(flashed.end(),b,b+n);return 0;}
int esp_ota_end(int){++ends;assert(flashed==expected);return 0;}
int esp_ota_abort(int){++aborts;return 0;}
struct mbedtls_sha256_context{std::vector<uint8_t>bytes;};
void mbedtls_sha256_init(mbedtls_sha256_context*p){p->bytes.clear();}
int mbedtls_sha256_starts_ret(mbedtls_sha256_context*,int){return 0;}
int mbedtls_sha256_update_ret(mbedtls_sha256_context*p,const uint8_t*b,int n){p->bytes.insert(p->bytes.end(),b,b+n);return 0;}
int mbedtls_sha256_finish_ret(mbedtls_sha256_context*p,uint8_t*d){memset(d,p->bytes==expected?0:1,32);return 0;}
void mbedtls_sha256_free(mbedtls_sha256_context*){}
'''+body+r'''
void reset(){ticks=0;s_cancel=false;s_written=0;s_err[0]=0;requests=begins=ends=aborts=0;resumeStatus=206;dropLimit=100;interrupted=alwaysDrop=badRange=badSize=corrupt=cancelRead=missingStream=false;rangeOffsets.clear();expected.resize(256);for(int i=0;i<256;++i)expected[i]=i;}
int main(){
 reset();assert(downloadAttempt()==AttemptResult::Success);assert(requests==1&&ends==1&&aborts==0);
 reset();interrupted=true;assert(downloadAttempt()==AttemptResult::Success);assert(requests==2&&begins==1&&ends==1&&aborts==0&&rangeOffsets==std::vector<int>{100});assert(flashed==expected);
 reset();interrupted=true;badRange=true;assert(downloadAttempt()==AttemptResult::Failed);assert(ends==0&&aborts==1&&flashed.size()==100);
 reset();interrupted=true;badSize=true;assert(downloadAttempt()==AttemptResult::Failed);assert(ends==0&&aborts==1);
 reset();interrupted=true;resumeStatus=200;assert(downloadAttempt()==AttemptResult::Failed);assert(ends==0&&aborts==1);
 reset();interrupted=true;resumeStatus=403;assert(downloadAttempt()==AttemptResult::Failed);assert(ends==0&&aborts==1);
 reset();interrupted=true;resumeStatus=500;assert(downloadAttempt()==AttemptResult::Retry);assert(ends==0&&aborts==1);
 reset();interrupted=true;missingStream=true;assert(downloadAttempt()==AttemptResult::Retry);assert(ends==0&&aborts==1);
 reset();interrupted=true;corrupt=true;assert(downloadAttempt()==AttemptResult::Failed);assert(ends==0&&aborts==1&&!strcmp(s_err,"firmware hash mismatch"));
 reset();alwaysDrop=true;s_release.size=256; // Three connections can finish this fixture.
 assert(downloadAttempt()==AttemptResult::Success);assert(requests==3&&begins==1&&ends==1);
 reset();alwaysDrop=true;dropLimit=1;assert(downloadAttempt()==AttemptResult::Retry);assert(requests==3&&begins==1&&ends==0&&aborts==1);
 reset();cancelRead=true;assert(downloadAttempt()==AttemptResult::Failed);assert(requests==1&&ends==0&&aborts==1);
 puts("PASS real OTA loop: bounded authenticated resume, exact suffix, hash failure, cancellation and cleanup");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
