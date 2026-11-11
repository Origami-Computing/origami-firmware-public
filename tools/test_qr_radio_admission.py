"""Execute production QR radio-stop/Scan bodies with failed Wi-Fi shutdown."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/hal/hal_wifi.cpp').read_text()
def block(start):
 end=s.index('{',start)+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
stop=block(re.search(r'static (?:bool|void) stopQrRadio\(\)',s).start())
scan=block(s.index('if (command == QrCommand::Scan) {'))
code=r'''
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
bool s_wifiSuspended=false,s_wasStaConnected=true,s_qrActive=false,s_qrJoining=true;
bool s_restorePending=true,s_connectPending=true,offOk=true,disconnectOk=true,ble=true;
unsigned s_qrId=0;char s_qrCredentials[32];
std::string trace;
constexpr int WIFI_OFF=0;
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms){assert(ms==80);trace+='T';}
enum class RadioState{Suspended};
void setRadioState(RadioState,const char*){trace+='S';}
namespace hal {namespace ble {bool started(){return ::ble;}void deinit(){trace+='B';}}}
struct {void setAutoReconnect(bool on){assert(!on);trace+='A';}
 bool disconnect(bool off){assert(off);trace+='D';return disconnectOk;}
 bool mode(int mode){assert(mode==WIFI_OFF);trace+='M';return offOk;}
} WiFi;
struct {void println(const char*){trace+='L';}} Serial;
enum class QrPhase{Preparing,Scanning,Failed};enum class QrError{None,Radio};
QrPhase phase;QrError error;
void publishQr(QrPhase p,QrError e=QrError::None){phase=p;error=e;trace+='P';}
'''+stop+'\nvoid scan(unsigned id) '+scan[scan.index('{'):]+r'''
int main(){
 for(bool bleOn:{false,true})for(bool disconnect:{false,true})for(bool off:{false,true}){
  ble=bleOn;disconnectOk=disconnect;offOk=off;trace.clear();
  s_restorePending=s_connectPending=true;memset(s_qrCredentials,1,sizeof(s_qrCredentials));
  scan(42);
  assert(s_qrActive&&s_qrId==42&&!s_qrJoining&&!s_qrCredentials[0]);
  assert(s_wifiSuspended&&!s_wasStaConnected&&!s_restorePending&&!s_connectPending);
  assert(phase==(off?QrPhase::Scanning:QrPhase::Failed));
  assert(error==(off?QrError::None:QrError::Radio));
  assert(trace==(std::string(ble?"SBADMT":"SADMT")+(off?"P":"LP")));
 }
 puts("PASS QR scanner admission requires Wi-Fi off success; disconnect failure alone does not veto successful stop");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
