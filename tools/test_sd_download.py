"""Check production SD download packets, partial-write abort and reservation cleanup."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/tooling/dev_serial_input.cpp').read_text()
def function(signature):
 a=source.index(signature);b=source.index('{',a)+1;depth=1
 while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
code=r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <algorithm>
#include <cassert>
int locks=0,resumes=0,closes=0;bool failRead=false;
namespace hal {namespace bus {void lockSpi(){assert(!locks);++locks;}void unlockSpi(){assert(locks);--locks;}}}
struct Reservation {bool held=true;void release(){assert(!locks);if(held){held=false;++resumes;}};} s_sdDownloadReservation;
struct File {bool alive=false;int at=0;explicit operator bool()const{return alive;}
 int read(uint8_t* p,size_t n){assert(locks&&alive);if(failRead)return 0;for(size_t i=0;i<n;++i)p[i]=uint8_t(at++);return int(n);}
 void close(){assert(locks&&alive);alive=false;++closes;}
};
struct SdDownloadState {bool active=false,backgroundPaused=false;File file;uint32_t size=0,sent=0,crc=0xffffffff;char path[64]="/videos/test.avi";} s_sdDownload;
struct SerialStub {
 std::string output;size_t limit=10000;int timeout=5,writes=0;
 void setTxTimeoutMs(int ms){timeout=ms;}
 size_t write(const uint8_t* p,size_t n){assert(!locks&&timeout==100);++writes;n=std::min(n,limit);output.append((const char*)p,n);return n;}
 void println(){output+='\n';}
 void printf(const char* f,...){char b[256];va_list args;va_start(args,f);vsnprintf(b,sizeof(b),f,args);va_end(args);output+=b;}
} Serial;
'''
for sig in ['static uint32_t crc32UpdateRaw(', 'static size_t base64Encode(', 'static void finishSdDownload(', 'static void pumpSdDownload(']:code+=function(sig)+'\n'
code+=r'''
void setup(){s_sdDownload={};s_sdDownload.active=s_sdDownload.backgroundPaused=s_sdDownload.file.alive=true;s_sdDownload.size=700;Serial={};resumes=closes=0;failRead=false;s_sdDownloadReservation.held=true;}
int main(){
 const uint8_t known[]="123456789";assert(~crc32UpdateRaw(0xffffffff,known,9)==0xcbf43926);
 char b[20];assert(base64Encode(known,1,b,sizeof(b))==4&&!strcmp(b,"MQ=="));
 assert(base64Encode(known,2,b,sizeof(b))==4&&!strcmp(b,"MTI="));
 assert(base64Encode(known,3,b,sizeof(b))==4&&!strcmp(b,"MTIz"));
 setup();pumpSdDownload();assert(s_sdDownload.sent==384&&Serial.writes==1&&Serial.timeout==5);
 pumpSdDownload();assert(s_sdDownload.sent==700&&Serial.writes==2&&Serial.timeout==5);
 pumpSdDownload();assert(!s_sdDownload.active&&closes==1&&resumes==1&&Serial.output.find("SDGET END size=700 crc=")!=std::string::npos);
 for(size_t limit:{0u,1u,100u,500u}){
  setup();Serial.limit=limit;pumpSdDownload();
  assert(!s_sdDownload.active&&closes==1&&resumes==1&&Serial.timeout==5);
  assert(Serial.output.find("reason=transmit")!=std::string::npos&&Serial.output.find("offset=0\n")!=std::string::npos);
  assert(Serial.output.find("SDGET END")==std::string::npos);
 }
 setup();failRead=true;pumpSdDownload();assert(!s_sdDownload.active&&closes==1&&resumes==1&&Serial.writes==0);
 assert(Serial.output.find("reason=read")!=std::string::npos);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-DARDUINO_USB_CDC_ON_BOOT=1','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production SD transfer: base64/CRC, one bounded packet, checked short writes, timeout restoration and cleanup')

# Exercise the actual host receiver without a physical serial dependency.
import base64,importlib.util,io,sys,types,zlib
sys.modules.setdefault('serial',types.ModuleType('serial'))
spec=importlib.util.spec_from_file_location('download_receiver',ROOT/'tools/camera_quality/run_settle_sweep.py')
receiver=importlib.util.module_from_spec(spec);spec.loader.exec_module(receiver)
class ResponseDevice(receiver.Device):
 def __init__(self,responses):self.responses=iter(responses)
 def send(self,command):assert command=='sdget /videos/test.avi'
 def line(self,deadline):return next(self.responses)
payload=bytes(range(256))*3
encoded=base64.b64encode(payload).decode()
crc=zlib.crc32(payload)
with tempfile.TemporaryDirectory() as directory:
 out=Path(directory)/'video.avi'
 lines=['unrelated log','prefix SDGET BEGIN size=768 path=/videos/test.avi',
        'partial log prefix SDGET DATA offset=0 bytes=768 '+encoded,
        f'SDGET END size=768 crc={crc:08x} path=/videos/test.avi']
 ResponseDevice(lines).download('/videos/test.avi',out);assert out.read_bytes()==payload;out.unlink()
 for bad in [lines[:2]+[lines[2].replace('offset=0','offset=1')]+lines[3:],
             lines[:3]+[f'SDGET END size=768 crc={crc^1:08x} path=/videos/test.avi']]:
  try:ResponseDevice(bad).download('/videos/test.avi',out)
  except RuntimeError:pass
  else:raise AssertionError('corrupt transfer accepted')
  assert not out.exists()
print('PASS actual host receiver: log-prefix framing, byte preservation, bad offsets and CRC rejection')
