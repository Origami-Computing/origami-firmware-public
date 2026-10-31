"""Exercise actual HAL writer functions with real stdio and injected failures.
Does not emulate FatFS, physical card removal, or power-loss durability.
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
#include <cstdint>
#include <climits>
#include <cerrno>
#include <cstdlib>
#include <new>
#include <unistd.h>
#include <string>
#include <vector>
#include <algorithm>
bool failAllocation=false,failStreamError=false,failOpen=false,failBuffer=false,failWrite=false,failSeek=false,failFlush=false,failSync=false,failClose=false;
#ifdef ORIGAMI_SD_SECTOR_WRITE_TRIAL
constexpr size_t bufferBytes=512;
#elif defined(ORIGAMI_SD_BATCH_WRITE_TRIAL)
constexpr size_t bufferBytes=16384;
#else
constexpr size_t bufferBytes=4096;
#endif
int locks=0,handles=0,flushes=0,syncs=0,closes=0;size_t maxWrite=0;
struct SerialStub {template<class... Args> void printf(const char*,Args...){assert(!locks);}} Serial;
void* operator new(size_t n,const std::nothrow_t&) noexcept {return failAllocation ? nullptr : ::operator new(n);}
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t n,int caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return failAllocation?nullptr:malloc(n);}
void heap_caps_free(void* p){free(p);}
int checkedError(FILE* f){assert(locks);return failStreamError ? 1 : ::ferror(f);}
namespace hal {namespace bus {
bool lockSpi(){++locks;return true;}void unlockSpi(){assert(locks);--locks;}
}}
FILE* checkedOpen(const char* p,const char* m){assert(locks);if(failOpen)return nullptr;auto* f=::fopen(p,m);if(f)++handles;return f;}
int checkedBuffer(FILE* f,char* b,int m,size_t n){assert(locks&&n==bufferBytes);return failBuffer ? -1 : ::setvbuf(f,b,m,n);}
size_t checkedWrite(const void* p,size_t size,size_t n,FILE* f){assert(locks&&size==1&&n<=bufferBytes);maxWrite=std::max(maxWrite,n);return failWrite ? 0 : ::fwrite(p,size,n,f);}
int checkedSeek(FILE* f,long p,int mode){assert(locks);return failSeek ? -1 : ::fseek(f,p,mode);}
int checkedFlush(FILE* f){assert(locks);++flushes;const int result=::fflush(f);return failFlush ? EOF : result;}
int checkedSync(int fd){assert(locks);++syncs;return failSync ? -1 : ::fsync(fd);}
int checkedClose(FILE* f){assert(locks);++closes;--handles;const int result=::fclose(f);return failClose ? EOF : result;}
#define ferror checkedError
#define fopen checkedOpen
#define setvbuf checkedBuffer
#define fwrite checkedWrite
#define fseek checkedSeek
#define fflush checkedFlush
#define fsync checkedSync
#define fclose checkedClose
namespace hal {namespace sd {
bool s_mounted=true;const char* kMountPath;
'''
code+=function(source,'struct WriteHandle')+';\n'
code+=function(header,'struct StreamWriter')+';\n'
for signature in ['bool openWriter(', 'bool writeStream(', 'bool seekWriter(', 'bool closeWriter(']:
    code+=function(source,signature)+'\n'
code+=r'''
}}
#undef ferror
#undef fopen
#undef setvbuf
#undef fwrite
#undef fseek
#undef fflush
#undef fsync
#undef fclose
int main(int argc,char** argv){assert(argc==2);using namespace hal::sd;kMountPath=argv[1];StreamWriter w;
 assert(closeWriter(nullptr)&&closeWriter(&w));
 assert(!openWriter(nullptr,&w)&&!openWriter("relative",&w));
 std::string longPath(257,'a');longPath[0]='/';assert(!openWriter(longPath.c_str(),&w));
 s_mounted=false;assert(!openWriter("/test.bin",&w));s_mounted=true;
 for(bool* failure:{&failAllocation,&failOpen,&failBuffer}){*failure=true;assert(!openWriter("/test.bin",&w)&&!w.handle&&!handles&&!locks);*failure=false;}
 assert(openWriter("/test.bin",&w)&&handles==1);assert(!openWriter("/test.bin",&w));
 std::vector<uint8_t> bytes(2*bufferBytes+12345);for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*17);
 assert(!writeStream(&w,nullptr,1));assert(writeStream(&w,nullptr,0));
 assert(writeStream(&w,bytes.data(),bytes.size())&&maxWrite==bufferBytes);
 assert(seekWriter(&w,4));uint8_t patch=0x42;assert(writeStream(&w,&patch,1));bytes[4]=patch;
 assert(closeWriter(&w)&&!w.handle&&!handles&&!locks);
 FILE* f=::fopen((std::string(kMountPath)+"/test.bin").c_str(),"rb");assert(f);
 std::vector<uint8_t> readback(bytes.size());assert(::fread(readback.data(),1,readback.size(),f)==bytes.size());::fclose(f);assert(readback==bytes);
 for(bool* failure:{&failStreamError,&failFlush,&failSync,&failClose}){
  assert(openWriter("/test.bin",&w));assert(writeStream(&w,bytes.data(),17));
  const int a=flushes,b=syncs,c=closes;*failure=true;
  assert(!closeWriter(&w)&&!w.handle&&!handles&&!locks);*failure=false;
  assert(flushes==a+1&&syncs==b+1&&closes==c+1);assert(closeWriter(&w));
 }
 assert(openWriter("/test.bin",&w));failWrite=true;assert(!writeStream(&w,bytes.data(),10));failWrite=false;
 failSeek=true;assert(!seekWriter(&w,0));failSeek=false;assert(closeWriter(&w));
 assert(!writeStream(&w,bytes.data(),1)&&!seekWriter(&w,0));
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    for flags in ([], ['-DORIGAMI_SD_SECTOR_WRITE_TRIAL'], ['-DORIGAMI_SD_BATCH_WRITE_TRIAL']):
        subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',*flags,str(root/'test.cpp'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test'),str(root)],check=True)
print('PASS production buffered SD writer: exact bytes, bounded writes, SPI ownership, flush/sync/close failures and cleanup under sanitizers')
