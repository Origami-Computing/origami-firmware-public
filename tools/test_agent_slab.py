"""Fault-inject the production PCM allocator without device memory pressure."""
from pathlib import Path
import argparse, subprocess,tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compare",type=Path,help="Compare allocation and diagnostic traces with a previous allocator source")
parser.add_argument("--seconds",type=int,choices=[6,12,20,60],default=20)
args=parser.parse_args()
capacities=list(dict.fromkeys(n for n in [args.seconds,20,12,6] if n<=args.seconds))
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text.replace("REQUESTED_SECONDS",str(args.seconds)).replace("ATTEMPTS",str(4*len(capacities))).replace("CAPACITIES",",".join(map(str,capacities))))
    write('Arduino.h','''#pragma once
#include <cstdio>
#include <string>
#include <regex>
struct Logger { template<class... T> void printf(const char* format,T... args){
 char text[512];std::snprintf(text,sizeof(text),format,args...);
 const auto normalized=std::regex_replace(std::string(text),std::regex("pcm=0x[0-9a-fA-F]+"),"pcm=ADDRESS");
 std::fputs(normalized.c_str(),stdout);
} }; extern Logger Serial;
''')
    write('config/app_config.h','''#define AGENT_STREAM_BACKLOG_SECONDS REQUESTED_SECONDS
#define AGENT_OUT_RATE 16000
''')
    write('esp_heap_caps.h','''#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void* heap_caps_malloc(size_t,uint32_t);
void heap_caps_free(void*);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_free_size(uint32_t);
''')
    write('main.cpp','''#include "core/agent_slab.h"
#include "Arduino.h"
#include <cassert>
#include <cstdlib>
#include <map>
#include <type_traits>
Logger Serial;int calls=0,successAt=1,frees=0;std::map<void*,void*> allocations;
void* heap_caps_malloc(size_t bytes,uint32_t caps){assert(caps&1);std::printf("allocate %zu %u\\n",bytes,caps);if(++calls!=successAt)return nullptr;
 auto base=static_cast<uint8_t*>(malloc(bytes+32));assert(base);auto ptr=base+4;allocations[ptr]=base;return ptr;}
void heap_caps_free(void*p){std::puts("free");assert(allocations.count(p)==1);free(allocations[p]);allocations.erase(p);++frees;}
size_t heap_caps_get_largest_free_block(uint32_t){return 7000000;}
size_t heap_caps_get_free_size(uint32_t){return 8000000;}
int main(){
 static_assert(!std::is_copy_constructible<core::AgentSlab>::value,"owner cannot be copied");
 static_assert(!std::is_copy_assignable<core::AgentSlab>::value,"owner cannot be assigned");
 core::AgentSlab slab;
 for(int allocation=1;allocation<=ATTEMPTS;++allocation){
  calls=0;successAt=allocation;int before=frees;
  assert(slab.acquire()&&slab.held()&&slab.pcm());
  const unsigned capacities[]={CAPACITIES};unsigned seconds=capacities[(allocation-1)/4];
  assert(slab.pcmMax()==seconds*16000);
  if((allocation-1)%4<2)assert(reinterpret_cast<uintptr_t>(slab.pcm())%16==0);
  int attempts=calls;assert(slab.acquire()&&calls==attempts);
  slab.pcm()[0]=1;slab.pcm()[slab.pcmMax()-1]=2;
  slab.release();assert(!slab.held()&&!slab.pcm()&&slab.pcmMax()==0&&allocations.empty()&&frees==before+1);
  slab.release();assert(frees==before+1);
 }
 calls=0;successAt=99;assert(!slab.acquire()&&calls==ATTEMPTS&&!slab.held()&&slab.pcmMax()==0&&allocations.empty());
 calls=0;successAt=1;assert(slab.acquire());slab.release();assert(allocations.empty());
}
''')
    def check(source, name):
        binary=root/name
        subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(source),str(root/'main.cpp'),'-o',str(binary)],check=True)
        return subprocess.check_output([str(binary)])
    actual=check(ROOT/'src/core/agent_slab.cpp','test')
    if args.compare:
        expected=check(args.compare,'baseline')
        assert actual==expected, 'Allocation/free/diagnostic trace differs from baseline'
        print('PASS identical allocation sizes/capabilities/order, frees and normalized diagnostics')
print(f'PASS {4*len(capacities)} allocation paths, {capacities}-second capacities, alignment, single-owner release, failure and recovery under sanitizers')
