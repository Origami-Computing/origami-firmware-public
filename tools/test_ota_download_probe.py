"""Run the serial OTA probe workflow with fake production owners; never activate."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/tooling/dev_serial_input.cpp').read_text()
workflow = source[source.index('static services::firmware_update::Session s_otaProbeSession;'):
                  source.index('static bool handleOtaAuthCommand(')]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
uint32_t clockMs=1000;
uint32_t millis(){return clockMs;}
struct esp_partition_t{uint32_t address;};
esp_partition_t boot{0x10000};
const esp_partition_t* esp_ota_get_boot_partition(){return &boot;}
std::string output;
struct Logger {
 template<class... T>void printf(const char* fmt,T... args){char b[1024];snprintf(b,sizeof(b),fmt,args...);output+=b;}
 void println(){output+='\n';}
} Serial;
bool busy=false,reserveOk=true,fetchOk=true,beginOk=true,running=true;
int reserves=0,closes=0,fetches=0,begins=0,written=0,total=100;
const char* error="";
namespace services {
namespace firmware_update {
bool noticeBusy(){return busy;}
struct Session {
 enum class Result{Ready,Busy};
 Result reserveNetwork(){++reserves;return reserveOk?Result::Ready:Result::Busy;}
 void close(){++closes;running=false;}
};
}
namespace ota_client {
enum class ReleaseChannel{Production};
struct ReleaseInfo{ReleaseChannel channel;char tag[32];int size;uint8_t sha256[32];};
ReleaseChannel releaseChannel(){return ReleaseChannel::Production;}
const char* releaseChannelName(ReleaseChannel){return "Production";}
bool fetchLatestRelease(ReleaseChannel,ReleaseInfo* out){++fetches;out->size=100;strcpy(out->tag,"v1.0.0");return fetchOk;}
bool beginFlash(const ReleaseInfo&){++begins;return beginOk;}
bool pumpFlash(int* w,int* t){*w=written;*t=total;return running;}
const char* lastError(){return error;}
// No finishFlash stub: any accidental activation fails compilation.
}}
''' + workflow + r'''
void reset(){
 s_otaProbeState=1;s_otaProbeStart=clockMs;s_otaProbeBoot=&boot;
 busy=false;reserveOk=fetchOk=beginOk=running=true;
 reserves=closes=fetches=begins=written=0;total=100;error="";output.clear();
}
int main(){
 reset();busy=true;pumpOtaProbe(clockMs);assert(!reserves&&!fetches&&s_otaProbeState==1);
 busy=false;pumpOtaProbe(clockMs);assert(reserves==1&&fetches==1&&begins==1&&s_otaProbeState==2);
 pumpOtaProbe(clockMs);assert(reserves==1&&fetches==1&&begins==1&&!closes);
 running=false;written=100;pumpOtaProbe(clockMs);
 assert(!s_otaProbeState&&closes==1&&output.find("VERIFIED boot_unchanged=1")!=std::string::npos);
 reset();reserveOk=false;pumpOtaProbe(clockMs);assert(!fetches&&!begins&&closes==1&&!s_otaProbeState);
 reset();fetchOk=false;pumpOtaProbe(clockMs);assert(!begins&&closes==1&&!s_otaProbeState);
 reset();beginOk=false;pumpOtaProbe(clockMs);assert(closes==1&&!s_otaProbeState);
 reset();pumpOtaProbe(clockMs);running=false;written=100;error="hash mismatch";pumpOtaProbe(clockMs);
 assert(closes==1&&output.find("PROBE FAILED")!=std::string::npos);
 reset();pumpOtaProbe(clockMs);running=false;written=99;pumpOtaProbe(clockMs);
 assert(closes==1&&output.find("PROBE FAILED")!=std::string::npos);
 reset();pumpOtaProbe(clockMs);closeOtaProbe("CANCELLED");assert(closes==1&&!s_otaProbeState);
 reset();clockMs+=180001;pumpOtaProbe(clockMs);assert(!reserves&&closes==1&&!s_otaProbeState);
 puts("PASS OTA probe success/failure, notice deferral, reservation, cancellation, deadline, boot slot retained; no activation API");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined', str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
