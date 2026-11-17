"""Exercise the actual canonical JPEG block callback bounds and cancellation."""

from pathlib import Path
import subprocess, tempfile

R = Path(__file__).resolve().parents[1]
s = (R / "src/services/cine_camera_resize.cpp").read_text()
s = s[
    s.index("struct Decode {") : s.index(
        "} // namespace\nvoid Session::releaseIspPreview()"
    )
]
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / "test.cpp").write_text(
        r"""#include <stdint.h>
#include <cstring>
#include <stddef.h>
#include <cassert>
#include <vector>
#define MAX_BUFFERED_PIXELS 2048
namespace core{namespace watchdog{void feed(){}}}
struct Session{static constexpr unsigned previewWidth=256,previewHeight=192;};
struct JPEGDRAW{void* pUser;int x,y,iWidthUsed,iHeight,iWidth;uint16_t* pPixels;};
"""
        + s
        + r"""
int main(){std::vector<uint16_t> dst(256*192+2,0xbeef),src(2048,0x1234);Decode c{dst.data()+1};c.valid=true;
for(int y=0;y<192;y+=8){JPEGDRAW b{&c,0,y,256,8,256,src.data()};assert(writeBlock565(&b));}
assert(dst.front()==0xbeef&&dst.back()==0xbeef);for(unsigned i=1;i+1<dst.size();++i)assert(dst[i]==0x1234);
for(auto b:std::vector<JPEGDRAW>{{&c,-1,0,256,8,256,src.data()},{&c,0,0,257,8,257,src.data()},{&c,0,191,256,8,256,src.data()},{&c,0,0,256,8,255,src.data()},{&c,0,0,256,9,256,src.data()}}){c.valid=true;assert(!writeBlock565(&b)&&!c.valid);}
// HD video uses a centered 4:3 region of its 320x180 reduced frame.
c.width=320;c.height=180;
std::vector<uint16_t> stripe(320);
for(unsigned x=0;x<320;++x)stripe[x]=x;
for(int y=0;y<180;++y){JPEGDRAW line{&c,0,y,320,1,320,stripe.data()};assert(writeBlock565(&line));}
for(unsigned y=0;y<192;++y)for(unsigned x=0;x<256;++x)
 assert(dst[1+y*256+x]==40+x*240/256);
assert(dst.front()==0xbeef&&dst.back()==0xbeef);
JPEGDRAW b{&c,0,0,256,8,256,src.data()};c.shouldAbort=[](void*){return true;};assert(!writeBlock565(&b)&&c.interrupted);assert(!writeBlock565(nullptr));}
"""
    )
    subprocess.run(
        [
            "c++",
            "-std=c++11",
            "-O1",
            "-fsanitize=address,undefined",
            str(p / "test.cpp"),
            "-o",
            str(p / "test"),
        ],
        check=True,
    )
    subprocess.run([str(p / "test")], check=True)
print(
    "PASS canonical JPEG RGB565 block mapper: complete coverage, guards, geometry, stride, capacity and cancellation; ASan/UBSan"
)
