"""A gallery upload action disappears only for a matching account and original."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    for name, body in {
        'services/video_upload_state.h': '''
namespace core { namespace secrets { inline int backendConfig(){return 1;} } }
namespace services { namespace video_upload { bool binding(int,char (&)[65]); } }
''',
        'hal/hal_sd.h': '''
#include <cstddef>
#include <cstdint>
namespace hal { namespace sd {
bool fileInfo(const char*,size_t&,uint32_t&);
size_t readFile(const char*,char*,size_t);
bool writeFile(const char*,const uint8_t*,size_t);
} }
''',
        'main.cpp': '''
#include "services/gallery_receipt.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
std::map<std::string,std::string> markers;
char owner='a'; uint32_t length=123, stamp=456;
namespace services { namespace video_upload {
bool binding(int,char (&out)[65]){memset(out,owner,64);out[64]=0;return true;}
} }
namespace hal { namespace sd {
bool fileInfo(const char*,size_t& size,uint32_t& m){size=length;m=stamp;return true;}
size_t readFile(const char* path,char* dst,size_t cap){
 auto& value=markers[path];auto n=value.copy(dst,cap-1);dst[n]=0;return n;
}
bool writeFile(const char* path,const uint8_t* src,size_t n){
 markers[path]=std::string(reinterpret_cast<const char*>(src),n);return true;
}
} }
int main(){
 using namespace services::gallery_receipt;
 char account[65];services::video_upload::binding(1,account);
 const char* path="/photos/IMG_123.jpg";assert(!confirmed(path));
 remember(path,account,length,stamp);assert(confirmed(path));
 assert(!confirmed("/videos/IMG_123.jpg"));
 ++length;assert(!confirmed(path));--length;
 ++stamp;assert(!confirmed(path));--stamp;
 owner='b';assert(!confirmed(path));owner='a';assert(confirmed(path));
 for(auto& pair:markers)pair.second.resize(pair.second.size()/2);
 assert(!confirmed(path));
}
''',
    }.items():
        file = path / name
        file.parent.mkdir(exist_ok=True, parents=True)
        file.write_text(body)
    exe = path / 'check'
    subprocess.run(['c++', '-std=c++11', '-I'+tmp, '-I'+str(root/'include'),
                    str(root/'src/services/gallery_receipt.cpp'), str(path/'main.cpp'),
                    '-o', str(exe)], check=True)
    subprocess.run([exe], check=True)
print('PASS gallery receipts: missing, delivered, changed file, changed account and torn marker')
