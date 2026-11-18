"""Exercise production metadata lookup with real files and stat/SPI faults."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/hal/hal_sd.cpp').read_text()
start = source.index('bool fileInfo(')
end = source.index('\nint listFiles(', start)
function = source[start:end]
code = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
bool mounted=true, lockOk=true, statOk=true;
int locks=0, calls=0;
namespace hal { namespace bus {
bool lockSpi(){if(!lockOk)return false;++locks;return true;}
void unlockSpi(){assert(locks==1);--locks;}
}}
int checkedStat(const char* path, struct stat* info){
 assert(locks==1);++calls;return statOk ? ::stat(path,info) : -1;
}
namespace hal { namespace sd {
bool& s_mounted=mounted;
constexpr char kMountPath[]=".";
'''
# Replace only the syscall, leaving the struct stat type intact.
code += function.replace('::stat(', 'checkedStat(')
code += r'''
}}
int main(int argc,char** argv){
 assert(argc==2 && chdir(argv[1])==0);
 using hal::sd::fileInfo;
 size_t bytes=999;uint32_t modified=123;
 assert(fileInfo("/photo.jpg",bytes,modified));
 struct stat expected;assert(::stat("./photo.jpg",&expected)==0);
 assert(bytes==7 && modified==uint32_t(expected.st_mtime) && calls==1 && locks==0);
 bytes=999;modified=123;
 assert(!fileInfo("/missing",bytes,modified));
 assert(!fileInfo("/folder",bytes,modified));
 statOk=false;assert(!fileInfo("/photo.jpg",bytes,modified));statOk=true;
 assert(bytes==999 && modified==123 && locks==0);
 int before=calls;
 mounted=false;assert(!fileInfo("/photo.jpg",bytes,modified));mounted=true;
 lockOk=false;assert(!fileInfo("/photo.jpg",bytes,modified));lockOk=true;
 assert(!fileInfo(nullptr,bytes,modified));
 assert(!fileInfo("relative.jpg",bytes,modified));
 assert(!fileInfo(("/"+std::string(300,'x')).c_str(),bytes,modified));
 assert(calls==before && locks==0 && bytes==999 && modified==123);
}
'''
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    (folder / 'photo.jpg').write_bytes(b'fixture')
    (folder / 'folder').mkdir()
    (folder / 'test.cpp').write_text(code)
    binary = folder / 'test'
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined',
                    str(folder / 'test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(folder)], check=True)
print('PASS fileInfo: single stat, exact metadata, failures preserve outputs, SPI cleanup')
