"""Run production SD stream functions against real files and faultable POSIX calls.

Checks byte/position semantics, SPI ownership and descriptor cleanup. Does not
emulate FatFS/card latency, media removal, or ESP32 memory capabilities.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/hal/hal_sd.cpp').read_text()
header=(ROOT/'include/hal/hal_sd.h').read_text()
def function(text, signature):
 a=text.index(signature);b=text.index('{',a)+1;depth=1
 while depth:
  depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
code=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <climits>
#include <cstdint>
#include <string>
#include <algorithm>
bool failAllocation=false,failOpen=false,failStat=false,failRead=false,failSeek=false;
int locks=0,descriptors=0,readCalls=0,seekCalls=0;size_t readLimit=SIZE_MAX;
void* operator new(size_t n,const std::nothrow_t&) noexcept {
 return failAllocation ? nullptr : ::operator new(n);
}
uint32_t micros(){static uint32_t t=0;return ++t;}
namespace hal {namespace bus {
void lockSpi(){++locks;}void unlockSpi(){assert(locks>0);--locks;}
}}
int checkedOpen(const char* path,int flags){assert(locks);if(failOpen)return -1;int fd=::open(path,flags);if(fd>=0)++descriptors;return fd;}
int checkedStat(int fd,struct stat* s){assert(locks);return failStat ? -1 : ::fstat(fd,s);}
ssize_t checkedRead(int fd,void* p,size_t n){assert(locks);++readCalls;return failRead ? -1 : ::read(fd,p,std::min(n,readLimit));}
off_t checkedSeek(int fd,off_t n,int origin){assert(locks);++seekCalls;return failSeek ? -1 : ::lseek(fd,n,origin);}
int checkedClose(int fd){assert(locks);--descriptors;return ::close(fd);}
#define open checkedOpen
#define fstat checkedStat
#define read checkedRead
#define lseek checkedSeek
#define close checkedClose
namespace hal {namespace sd {
bool s_mounted=true;const char* kMountPath;
'''
code+=function(source,'struct ReadHandle')+';\n'
code+=function(header,'struct StreamReader')+';\n'
for signature in ['bool openStream(', 'size_t readStream(', 'bool seekStream(', 'void closeStream(']:
 code+=function(source,signature)+'\n'
code+=r'''
}}
#undef open
#undef fstat
#undef read
#undef lseek
#undef close
int main(int argc,char** argv){assert(argc==2);using namespace hal::sd;kMountPath=argv[1];
 StreamReader r;uint8_t bytes[9000];
 s_mounted=false;assert(!openStream("/media.bin",&r));s_mounted=true;
 assert(!openStream(nullptr,&r)&&!openStream("relative",&r)&&!openStream("/missing",&r));
 assert(!openStream("/",&r)&&!descriptors);
 std::string longPath(300,'x');longPath[0]='/';assert(!openStream(longPath.c_str(),&r));
 for(bool* failure:{&failOpen,&failStat,&failAllocation}){
  *failure=true;assert(!openStream("/media.bin",&r)&&!r.handle&&!descriptors&&!locks);*failure=false;
 }
 assert(openStream("/media.bin",&r)&&r.size==20000&&r.position==0&&descriptors==1);
 assert(!openStream("/media.bin",&r)&&descriptors==1);
 assert(readStream(&r,bytes,8192)==8192&&r.position==8192&&readCalls==1);
 for(size_t i=0;i<8192;++i)assert(bytes[i]==uint8_t(i*17));
 for(size_t offset:{19000u,12u,16000u,512u,19999u}){
  assert(seekStream(&r,offset)&&r.position==offset);
  assert(readStream(&r,bytes,1)==1&&bytes[0]==uint8_t(offset*17)&&r.position==offset+1);
 }
 assert(!readStream(&r,bytes,1)&&r.position==r.size);
 const int seeks=seekCalls;assert(!seekStream(&r,r.size+1)&&seekCalls==seeks);
 assert(seekStream(&r,20));failSeek=true;assert(!seekStream(&r,30)&&r.position==20);failSeek=false;
 failRead=true;assert(!readStream(&r,bytes,50)&&r.position==20);failRead=false;
 readLimit=7;assert(readStream(&r,bytes,50)==7&&r.position==27);
 for(size_t i=0;i<7;++i)assert(bytes[i]==uint8_t((20+i)*17));
 assert(!readStream(&r,nullptr,1)&&!readStream(&r,bytes,0));
 assert(r.readIoUs&&r.seekIoUs);closeStream(&r);
 assert(!descriptors&&!locks&&!r.handle&&!r.size&&!r.position&&!r.readIoUs&&!r.seekIoUs);
 closeStream(&r);assert(!readStream(&r,bytes,1)&&!seekStream(&r,0));
 // Audio retains the recursive bus across a bounded header/demux batch.
 hal::bus::lockSpi();assert(openStream("/media.bin",&r));
 assert(readStream(&r,bytes,7)==7 && seekStream(&r,100));
 closeStream(&r);assert(locks==1&&!descriptors);hal::bus::unlockSpi();
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'media.bin').write_bytes(bytes((i*17)%256 for i in range(20000)))
 (p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-DORIGAMI_DEV_SERIAL_INPUT','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p)],check=True)
print('PASS production SD streams: exact bytes, forward/backward seek, EOF, short/error reads, allocation/stat/open rollback and SPI ownership')
