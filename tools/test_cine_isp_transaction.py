"""Exercise the canonical HAL ISP transaction with a failure-injected register bus."""

from pathlib import Path
import subprocess, tempfile

R = Path(__file__).resolve().parents[1]
s = (R / "src/hal/hal_camera.cpp").read_text()
code = s[
    s.index("namespace {\nbool jpegShutterRead(") : s.index("\nbool triggerAutofocus(")
]
with tempfile.TemporaryDirectory() as td:
    p = Path(td)
    (p / "Arduino.h").write_text(
        "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n"
    )
    (p / "test.cpp").write_text(
        r"""
#include "hal/hal_camera.h"
#include "core/cine_shutter.h"
#include "core/cine_isp.h"
#include <cassert>
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
constexpr int PIXFORMAT_JPEG=4,FRAMESIZE_QXGA=17,FRAMESIZE_QVGA=5;
struct Sensor{int pixformat=PIXFORMAT_JPEG,size=0;int set_framesize(Sensor*,int next){size=next;return 0;}};Sensor sensor;
Sensor* esp_camera_sensor_get(){return &sensor;}
namespace hal{namespace bus{bool lockI2c(unsigned){return true;}void unlockI2c(){}}namespace camera{
bool s_qxgaBuffers=false,s_initialized=true;unsigned s_detectedPid=0x3660;void* s_lastFb=nullptr;constexpr unsigned kI2cLockTimeout=10;
std::array<uint8_t,65536> registers{};std::vector<unsigned> writes;int failRegister=-1,failReadRegister=-1;
bool ovReadReg(uint16_t a,uint8_t* out){if(a==failReadRegister)return false;*out=registers[a];return true;}
bool ovWriteReg(uint16_t a,uint8_t v){writes.push_back(a);if(a==failRegister){failRegister=-1;return false;}registers[a]=v;return true;}
"""
        + code
        + r"""
}}
int main(){using namespace hal::camera;
 assert(!setStillPreviewSize(true));s_qxgaBuffers=true;
 assert(setStillPreviewSize(false)&&sensor.size==FRAMESIZE_QVGA);
 s_lastFb=&sensor;assert(!setStillPreviewSize(true)&&sensor.size==FRAMESIZE_QVGA);s_lastFb=nullptr;
 assert(setStillPreviewSize(true)&&sensor.size==FRAMESIZE_QXGA);
 registers[0x303b]=24;registers[0x303c]=0x11;registers[0x303d]=0x30;
 registers[0x380c]=2300>>8;registers[0x380d]=2300&255;registers[0x380e]=1564>>8;registers[0x380f]=1564&255;
 registers[0x350b]=16;registers[0x3821]=0x20;registers[0x5000]=0xa7;registers[0x5001]=0xa3;
 auto initial=registers;
 CineIspRequest request;request.shutterUs=80000;CineIspReadback output;
 assert(configureCineIsp(request,&output)&&output.valid&&output.shutter.manual&&output.shutter.clockHz==48000000);
 assert(output.shutter.actualUs>=79997&&output.shutter.actualUs<=80003&&output.shutter.gainQ4==16&&output.shutter.vts>1564);
 assert(!memcmp(output.gamma,core::cine::gamma,16)&&!memcmp(output.cmx,core::cine::matrix,11));
 assert((output.flags[0]&0x80)==0&&(output.flags[0]&0x21)==0x21&&(output.flags[1]&0x80)==0&&(output.flags[1]&3)==3);
 auto successful=registers;auto regs=writes;for(auto reg:regs){registers=initial;auto before=registers;writes.clear();failRegister=reg;bool ok=configureCineIsp(request,&output);if(ok||registers!=before)fprintf(stderr,"rollback reg=%x ok=%d auto=%d\n",reg,ok,request.autoExposure);assert(!ok&&registers==before);}
 registers=successful;writes.clear();auto before=registers;ShutterInfo measured;assert(readJpegShutter(&measured)&&measured.manual&&measured.actualUs==output.shutter.actualUs&&writes.empty()&&registers==before);
 const uint16_t expected[3]={0x5e0,0x410,0x540};
 for(unsigned mode=0;mode<core::cine::colorCount;++mode){writes.clear();request.colorMode=mode;assert(configureCineIsp(request,&output));for(unsigned c=0;c<3;++c)assert(output.wb[c]==expected[c]);assert(registers[0x3406]==1);const auto& p=core::cine::colorProfile(core::cine::ColorMode(mode));assert(output.colorMode==mode&&!memcmp(output.gamma,p.gamma,16)&&!memcmp(output.cmx,p.matrix,11));
 // Every profile has the same rollback guarantee, including its distinct gamma/CMX.
 auto modeWrites=writes;for(auto reg:modeWrites){auto before=registers;failRegister=reg;assert(!configureCineIsp(request,&output)&&registers==before);}}
 for(unsigned mode:{core::cine::colorCount,255u}){request.colorMode=mode;writes.clear();assert(!configureCineIsp(request,&output)&&writes.empty());}request.colorMode=0;
 for(unsigned us:{999u,100001u}){request.shutterUs=us;assert(!configureCineIsp(request,&output));}request.shutterUs=10000;
 for(unsigned g:{15u,129u}){request.gainQ4=g;assert(!configureCineIsp(request,&output));}request.gainQ4=128;assert(configureCineIsp(request,&output)&&output.shutter.gainQ4==128);request.gainQ4=16;
 for(unsigned multiplier:{24u,30u}){registers[0x303b]=multiplier;uint32_t prior=0;for(auto us:core::cine::shutterSpeedsUs){request.shutterUs=us;assert(configureCineIsp(request,&output));assert(output.shutter.actualUs>prior&&output.shutter.actualUs<=100000);prior=output.shutter.actualUs;}}
 registers[0x303b]=24;request.autoExposure=true;request.autoVts=1564;assert(configureCineIsp(request,&output)&&!output.shutter.manual&&output.shutter.vts==1564&&output.shutter.requestedUs==0);
 assert((registers[0x3a00]&5)==0&&registers[0x3a18]==0&&registers[0x3a19]==128);
 assert(registers[0x3a0f]==48&&registers[0x3a10]==41&&registers[0x3a1b]==48&&registers[0x3a1e]==41&&registers[0x3a11]==96&&registers[0x3a1f]==20);
 assert(((registers[0x3a02]<<8)|registers[0x3a03])==1560);
 regs=writes;for(auto reg:regs){auto before=registers;failRegister=reg;bool ok=configureCineIsp(request,&output);if(ok||registers!=before)fprintf(stderr,"rollback reg=%x ok=%d auto=%d\n",reg,ok,request.autoExposure);assert(!ok&&registers==before);}
 // Color changes preserve live sub-millisecond Auto exposure and gain above the old 4x cap.
 registers[0x350b]=80;assert(configureCineIsp(request,&output)&&output.shutter.gainQ4==80);
 registers[0x350b]=144;assert(configureCineIsp(request,&output)&&output.shutter.gainQ4==128);
 registers[0x3500]=0;registers[0x3501]=1;registers[0x3502]=0x23;request.colorMode=4;assert(configureCineIsp(request,&output)&&output.shutter.ticks==0x123&&!output.shutter.manual);
 for(auto bad:{1,2,3,4}){writes.clear();s_lastFb=bad==1?(void*)1:nullptr;s_detectedPid=bad==2?0x5640:0x3660;sensor.pixformat=bad==3?0:4;s_initialized=bad!=4;assert(!configureCineIsp(request,&output)&&writes.empty());}
}
"""
    )
    subprocess.run(
        [
            "c++",
            "-std=c++17",
            "-O1",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-I" + str(p),
            "-I" + str(R / "include"),
            str(p / "test.cpp"),
            "-o",
            str(p / "test"),
        ],
        check=True,
    )
    subprocess.run([str(p / "test")], check=True)
print(
    "PASS canonical ISP: exact gamma/vendor+1 bytes, six fixed-Sunny Color modes, both JPEG clocks/shutter ladder, Auto bounds/live integration, ownership guards and every-register rollback; ASan/UBSan"
)
