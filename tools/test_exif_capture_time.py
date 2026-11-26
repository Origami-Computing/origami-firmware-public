"""Round-trip the camera's EXIF writer (src/core/cine_exif.cpp) through the
gallery's reader (include/core/exif_capture_time.h), plus malformed inputs.
Uses $CXX, else c++, else `python -m ziglang c++`.
"""
from pathlib import Path
import os, shlex, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
SOURCE = r'''#include "core/cine_exif.h"
#include "core/exif_capture_time.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace core;
static size_t jpegWith(uint8_t* out, uint32_t unix) {
 uint8_t app1[cine::exifCapacity];
 const cine::ExifInfo info{"v0.1.test","Natural",8000,1600,1200,true,unix};
 const size_t n=cine::buildExif(app1,sizeof app1,info); assert(n>0);
 out[0]=0xFF;out[1]=0xD8;memcpy(out+2,app1,n);
 out[2+n]=0xFF;out[3+n]=0xDA;out[4+n]=0;out[5+n]=2;  // start of scan
 return n+6;
}
int main(){
 uint8_t jpeg[1024];exif::CaptureTime t{};
 // 2026-11-24 16:12:05 UTC
 const uint32_t unix=1795536725u;
 size_t n=jpegWith(jpeg,unix);
 assert(exif::readCaptureTime(jpeg,n,t));
 assert(t.year==2026&&t.month==11&&t.day==24&&t.hour==16&&t.minute==12&&t.second==5);
 assert(t.hasOffset&&t.offsetMinutes==0);
 assert(exif::toUnix(t)==int64_t(unix));
 // Unknown clock: the camera omits the date, so nothing is shown.
 n=jpegWith(jpeg,0);
 assert(!exif::readCaptureTime(jpeg,n,t));
 // Truncated / not a JPEG / garbage never read out of bounds.
 n=jpegWith(jpeg,unix);
 for(size_t cut=0;cut<n;++cut){exif::CaptureTime x{};(void)exif::readCaptureTime(jpeg,cut,x);}
 const uint8_t junk[]={0xFF,0xD8,0xFF,0xE1,0xFF,0xFF,'E','x'};
 assert(!exif::readCaptureTime(junk,sizeof junk,t));
 assert(!exif::readCaptureTime(nullptr,0,t));
 // Offsets and local time from other cameras.
 int m=0; const uint8_t off[]="-07:30"; assert(exif::parseOffset(off,6,m)&&m==-450);
 const uint8_t dt[]="2025:01:03 09:41:00"; exif::CaptureTime c{};
 assert(exif::parseDateTime(dt,19,c)&&c.year==2025&&c.hour==9&&c.minute==41);
 const uint8_t bad[]="2025:13:03 09:41:00"; assert(!exif::parseDateTime(bad,19,c));
 assert(exif::daysFromCivil(1970,1,1)==0&&exif::daysFromCivil(2000,3,1)==11017);
 puts("PASS");
}
'''


def compiler():
    if os.environ.get("CXX"):
        return shlex.split(os.environ["CXX"].replace("\\", "/"))
    if shutil.which("c++"):
        return ["c++"]
    return [sys.executable, "-m", "ziglang", "c++"]


with tempfile.TemporaryDirectory() as td:
    folder = Path(td)
    (folder / "main.cpp").write_text(SOURCE)
    # Host-only: Windows has gmtime_s instead of POSIX gmtime_r.
    (folder / "shim.h").write_text(
        "#include <time.h>\n#ifdef _WIN32\n"
        "static inline struct tm* gmtime_r(const time_t* t, struct tm* r) { return gmtime_s(r, t) == 0 ? r : 0; }\n"
        "#endif\n")
    exe = folder / ("test.exe" if os.name == "nt" else "test")
    subprocess.run(compiler() + ["-std=c++17", "-Wall", "-Wextra", "-include", str(folder / "shim.h"), "-I" + str(root / "include"),
                                 str(folder / "main.cpp"), str(root / "src/core/cine_exif.cpp"), "-o", str(exe)],
                   check=True, stderr=subprocess.DEVNULL)
    output = subprocess.check_output([str(exe)], text=True)
    assert output.strip() == "PASS", output
print("PASS EXIF capture time: camera round trip, undated photos, truncation, offsets")
