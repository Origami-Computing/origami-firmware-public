"""Exercise real status-bar paint caching across canvas/direct-panel targets.

--source-ref REF --expect-stale reproduces the older stale-canvas behavior.
Normal mode requires a repaint after switching destinations, while stable-target
idle frames remain cached. Sensors and pixels are controlled host facades.
"""
import argparse,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-ref');p.add_argument('--expect-stale',action='store_true');args=p.parse_args()
def read(path):return subprocess.check_output(['git','show',args.source_ref+':'+path],cwd=ROOT,text=True) if args.source_ref else (ROOT/path).read_text()
src=read('src/ui/system_status_bar.cpp');header=read('include/ui/system_status_bar.h')
def fn(name):
 m=re.search(r'(?:void|bool) SystemStatusBar::'+name+r'\([^)]*\)\s*\{',src);assert m
 i=m.end();depth=1
 while depth:depth+=(src[i]=='{')-(src[i]=='}');i+=1
 return src[m.start():i]+'\n'
code=r'''
#include "ui/system_status_bar.h"
#include "ui/battery_indicator.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
class TFT_eSPI {public:int paints=0;uint16_t pixels=0;int width(){return 280;}void fillRect(int x,int y,int w,int h,uint16_t color){assert(y==0&&h==38&&x>=0&&x+w<=280);++paints;pixels=color;}};
static TFT_eSPI physicalPanel;static int transfers=0,locks=0;static bool allocating=true;
class TFT_eSprite:public TFT_eSPI {bool ready=false;public:TFT_eSprite(TFT_eSPI*){}bool created(){return ready;}void setColorDepth(int){}void* createSprite(int w,int h){assert(w==280&&h==38);ready=allocating;return ready?this:nullptr;}void pushSprite(int x,int y){assert(locks==1&&x==0&&y==0);++transfers;}void pushSprite(int x,int y,int sx,int sy,int w,int h){assert(locks==1&&x==sx&&y==sy&&h==38&&x+w<=280);++transfers;}};
constexpr int kSystemStatusBarH=38;uint32_t now=1000;uint32_t millis(){return now;}
bool external=true,wifiConnected=true,bleConnected=false;int tier=4;char clockText[8]="10:16";uint8_t percent=95;
namespace hal{namespace power{struct Status{uint8_t battery_percent=95;bool battery_estimated=false;};Status read(){Status p;p.battery_percent=percent;return p;}uint8_t batterySegments(Status){return 4;}bool externalPowerPresent(Status){return external;}bool charging(Status){return true;}}
namespace wifi{bool connected(){return wifiConnected;}}namespace ble{bool clientConnected(){return bleConnected;}}namespace display{void markDirty(int,int){}void markDirtyRect(int,int,int,int){}TFT_eSPI& directTft(){return physicalPanel;}void beginDirectDraw(){assert(locks++==0);}void endDirectDraw(){assert(--locks==0);}}}
namespace core{namespace time{void formatClockHHMM(char* b,size_t n){snprintf(b,n,"%s",clockText);}}}
namespace services{namespace automation{bool activeAlertUsesRedStatusBar(){return false;}}}
namespace services{namespace device_fs{struct SyncStatus{enum class Direction:uint8_t{None,Upload,Download};bool active=false,ok=true;uint32_t remaining=0;Direction direction=Direction::None;};SyncStatus syncState;void getSyncStatus(SyncStatus& s){s=syncState;}}}
struct UploadQueue{struct Counts{int pending=0,active=0,failed=0,retrying=0;};Counts state;Counts counts()const{return state;}};
UploadQueue uploadQueue;bool queueReady=false;
namespace services{namespace uploads{UploadQueue* readyQueue(){return queueReady?&uploadQueue:nullptr;}}}
namespace ui{
enum BarPart : uint8_t { WifiPart=1, BatteryPart=2, ClockPart=4, AllParts=7 };
uint32_t batteryAnimationTime(){return millis();}
hal::power::Status batteryStatusForPaint(){return hal::power::read();}
int8_t wifiTierRawFromEnv(){return tier;}int8_t wifiTierForPaint(bool,uint32_t,int8_t tier){return tier;}
static int clocks=0,wifis=0,batteries=0;
static uint8_t syncCount=0,syncDirection=0,syncFrame=0;
void drawBarGlyphs(TFT_eSPI&,int,int,uint8_t,uint8_t,bool,bool,bool,const char*,int8_t,bool,bool,bool,uint8_t count,uint8_t direction,uint8_t frame,uint16_t,bool=false,uint8_t parts=AllParts){
 if(parts&ClockPart)++clocks;if(parts&WifiPart)++wifis;if(parts&BatteryPart)++batteries;
 if(parts&WifiPart){syncCount=count;syncDirection=direction;syncFrame=frame;}
}
'''
if args.source_ref:
 code=code.replace('uint8_t count,uint8_t direction,uint8_t frame,uint16_t','uint16_t')
 code=code.replace(' if(parts&WifiPart){syncCount=count;syncDirection=direction;syncFrame=frame;}','')
code+=fn('invalidate')+fn('setBackgroundColor')
if not args.source_ref: code+=src[src.index('struct SyncIndicatorState'):src.index('bool SystemStatusBar::paint')]
code+=fn('paint')+(fn('paintDirect') if not args.source_ref else '')+'}\n'
code+=r'''
int main(){
 for(uint16_t alert:{uint16_t(0xf800),uint16_t(0x1234)}){
  ui::SystemStatusBar bar;TFT_eSPI canvas,panel;
  bar.paint(canvas);assert(canvas.pixels==0xffff&&canvas.paints==1);
  bar.paint(canvas);assert(canvas.paints==1); // Idle remains cheap.
  bar.setBackgroundColor(alert);bar.paint(canvas);assert(canvas.pixels==alert);
  bar.setBackgroundColor(0xffff);bar.invalidate();bar.paint(panel);
  assert(panel.pixels==0xffff&&panel.paints==1);
  bar.paint(panel);assert(panel.paints==1);
  bar.paint(canvas);
'''
code+='assert(canvas.pixels==alert&&canvas.paints==2);\n' if args.expect_stale else 'assert(canvas.pixels==0xffff&&canvas.paints==3);\n'
if not args.expect_stale:code+='bar.paint(canvas);assert(canvas.paints==3);bar.paint(panel);assert(panel.paints==2);bar.invalidate();bar.paint(panel);assert(panel.paints==3);\n'
code+='}\n'
if not args.source_ref:
 code+=r'''ui::SystemStatusBar direct;
 allocating=false;direct.paintDirect();assert(transfers==0); // No partial clear on allocation failure.
 allocating=true;direct.paintDirect();assert(transfers==1);
 direct.paintDirect();assert(transfers==1); // Cached strip makes no bus transfer.
 direct.invalidate();direct.paintDirect();assert(transfers==2);
 direct.setBackgroundColor(0x1234);direct.paintDirect();assert(transfers==3);
 const int oldClocks=ui::clocks,oldWifi=ui::wifis,oldBattery=ui::batteries;
 now+=300;direct.paintDirect();assert(transfers==4);
 assert(ui::clocks==oldClocks&&ui::wifis==oldWifi&&ui::batteries==oldBattery+1); // Charging animation remains live.
 // Hold time fixed while the bolt breathes. No Wi-Fi/clock repaint for 10 seconds.
 const int stableClock=ui::clocks,stableWifi=ui::wifis;
 for(int i=0;i<167;++i){now+=60;direct.paintDirect();}
 assert(ui::clocks==stableClock&&ui::wifis==stableWifi);
 strcpy(clockText,"10:17");direct.paintDirect();assert(ui::clocks==stableClock+1);
 int b=ui::batteries;percent=70;direct.paintDirect();assert(ui::batteries==b+1&&ui::clocks==stableClock+1);
 external=false;direct.paintDirect();b=ui::batteries;
 for(int i=0;i<100;++i){now+=60;direct.paintDirect();}assert(ui::batteries==b);
 wifiConnected=false;direct.paintDirect();assert(ui::wifis==stableWifi+1&&ui::clocks==stableClock+1);
 bleConnected=true;direct.paintDirect();assert(ui::wifis==stableWifi+2);
 percent=5;direct.paintDirect();b=ui::batteries;now+=600;direct.paintDirect();assert(ui::batteries==b+1&&ui::clocks==stableClock+1);
 direct.invalidate();direct.paintDirect();assert(ui::clocks==stableClock+2); // Restored background needs glyphs.
 assert(physicalPanel.paints==0&&locks==0); // All clears happen off-panel.
 ui::SystemStatusBar syncBar;TFT_eSPI syncCanvas;wifiConnected=true;queueReady=true;
 services::device_fs::syncState.remaining=2;services::device_fs::syncState.active=true;
 services::device_fs::syncState.direction=services::device_fs::SyncStatus::Direction::Upload;
 uploadQueue.state.pending=1;syncBar.paint(syncCanvas);
 assert(ui::syncCount==3&&ui::syncDirection==1);
 const int syncPaints=ui::wifis;now+=100;syncBar.paint(syncCanvas);
 assert(ui::wifis==syncPaints+1&&ui::syncFrame!=0);
 services::device_fs::syncState.direction=services::device_fs::SyncStatus::Direction::Download;
 syncBar.paint(syncCanvas);assert(ui::syncCount==3&&ui::syncDirection==2);
 services::device_fs::syncState.remaining=0;services::device_fs::syncState.active=false;uploadQueue.state.pending=0;
 syncBar.paint(syncCanvas);assert(ui::syncCount==0&&ui::syncDirection==0);
 services::device_fs::syncState.remaining=2;services::device_fs::syncState.ok=false;
 syncBar.paint(syncCanvas);assert(ui::syncCount==2&&ui::syncDirection==3);
 services::device_fs::syncState.ok=true;wifiConnected=false;
 syncBar.paint(syncCanvas);assert(ui::syncCount==2&&ui::syncDirection==3);
 wifiConnected=true;services::device_fs::syncState.remaining=0;uploadQueue.state.pending=2;
 syncBar.paint(syncCanvas);assert(ui::syncCount==2&&ui::syncDirection==1&&ui::syncFrame==15);
 uploadQueue.state.active=1;syncBar.paint(syncCanvas);
 assert(ui::syncCount==3&&ui::syncDirection==1);
 uploadQueue.state.retrying=1;syncBar.paint(syncCanvas);
 assert(ui::syncCount==3&&ui::syncDirection==3&&ui::syncFrame==0);
 uploadQueue.state.retrying=0;uploadQueue.state.failed=1;syncBar.paint(syncCanvas);
 assert(ui::syncCount==4&&ui::syncDirection==3);
 uploadQueue.state.failed=0;syncBar.paint(syncCanvas);
 assert(ui::syncCount==3&&ui::syncDirection==1);
 uploadQueue.state={};wifiConnected=false;syncBar.paint(syncCanvas);
 assert(ui::syncCount==0);
 '''
code+='puts("'+('REPRODUCED stale canvas after direct-panel paint' if args.expect_stale else 'PASS destination changes repaint; same-target idle cached; explicit invalidation repaints')+'");}\n'
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'ui').mkdir();(p/'ui/system_status_bar.h').write_text(header);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
