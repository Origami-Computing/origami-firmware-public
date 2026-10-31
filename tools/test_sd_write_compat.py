"""Exercise the exact pinned, patched SD multi-block write function with SPI fakes."""
from pathlib import Path
import importlib.util, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('sd_overlay',ROOT/'scripts/sd_write_compat.py')
overlay=importlib.util.module_from_spec(spec);spec.loader.exec_module(overlay)
framework=Path.home()/'.platformio/packages/framework-arduinoespressif32/libraries/SD/src/sd_diskio.cpp'
source=framework.read_bytes()
patched=overlay.patched_source(source).decode()
assert framework.read_bytes()==source
try:overlay.patched_source(source+b'\n')
except RuntimeError:pass
else:raise AssertionError('Unknown SDK source was accepted')
start=patched.index('bool sdWriteSectors(');end=patched.index('\nunsigned long sdGetSectorsCount',start)
function=patched[start:end]
code=r'''
#include <cassert>
#include <cstdint>
#include <string>
enum {CARD_MMC, CARD_SDHC, SET_WR_BLK_ERASE_COUNT, WRITE_BLOCK_MULTIPLE,
      SEND_STATUS, STOP_TRANSMISSION, SEND_NUM_WR_BLOCKS};
std::string trace;bool afterStop=false,allowStopReady=true,failBeforeStop=false;
int statusFailure=0,blockCount=0;
struct SPI {uint8_t transfer(uint8_t byte){assert(byte==0xff&&afterStop);trace+='D';return 0xff;}} spi;
struct ardu_sdcard_t {int type=CARD_SDHC;SPI* spi;};
ardu_sdcard_t card{CARD_SDHC,&spi};ardu_sdcard_t* s_cards[]={&card};
char sdTransaction(uint8_t,char cmd,unsigned int,unsigned int* resp){
 if(cmd==SET_WR_BLK_ERASE_COUNT)return 0;
 assert(cmd==SEND_STATUS&&trace=="PSDWX");trace+='C';*resp=statusFailure;return 0;
}
bool sdSelectCard(uint8_t){return true;}
char sdCommand(uint8_t,char cmd,unsigned int,unsigned int*){assert(cmd==WRITE_BLOCK_MULTIPLE);return 0;}
char sdWriteBytes(uint8_t,const char*,char token){assert(token==char(0xfc));++blockCount;return 5;}
bool sdWait(uint8_t,int timeout){assert(timeout==500);
 if(!afterStop){trace+='P';return !failBeforeStop;}
 assert(trace=="PSD");trace+='W';return allowStopReady;
}
void sdStop(uint8_t){assert(trace=="P");afterStop=true;trace+='S';}
void sdDeselectCard(uint8_t){trace+='X';}
bool sdReadBytes(uint8_t,char*,int){assert(false);return false;}
'''+function+r'''
void reset(){trace.clear();afterStop=false;allowStopReady=true;failBeforeStop=false;statusFailure=0;blockCount=0;}
int main(){char bytes[4096]={};
 reset();assert(sdWriteSectors(0,bytes,10,8));assert(trace=="PSDWXC"&&blockCount==8);
 reset();allowStopReady=false;assert(!sdWriteSectors(0,bytes,10,8));assert(trace=="PSDWX");
 reset();statusFailure=1;assert(!sdWriteSectors(0,bytes,10,8));assert(trace=="PSDWXC");
 reset();failBeforeStop=true;assert(!sdWriteSectors(0,bytes,10,8));assert(trace=="PX");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS pinned SD source guard, STOP dummy clock/busy ordering, timeout cleanup and retained status-error checks')
