"""Exercise boot-probe failure and later game acquisition using the production lifecycle."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]

fixture = r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include "services/motion.h"
struct Bus {
    bool online=false;
    void setClock(int) {} void setTimeOut(int) {} void beginTransmission(int) {}
    void write(int) {} int endTransmission(bool) {return online ? 0 : 2;}
    int requestFrom(uint8_t,uint8_t) {return 1;} int read(){return 0x6a;}
} Wire1;
struct Console {template<class... T> void printf(T...){} void println(const char*){}} Serial;
enum {ACC_SDA_PIN=4,ACC_SCL_PIN=5,LSM6DSL_ACC_GYRO_I2C_ADDRESS_HIGH=0xd6,
      LSM6DSL_STATUS_OK=0,LSM6DSL_ACC_GYRO_INT1_PEDO_DISABLED=0,
      LSM6DSL_INT2_PIN=2,LSM6DSL_INT1_PIN=1,kTapThreshold=12,kTapShockTime=2,
      kTapQuietTime=1,kWakeUpDur=2,kWakeUpThreshold=8,MALLOC_CAP_SPIRAM=0};
int digitalRead(int){return 1;} void delay(int){}
bool beginFails=false; int objects=0, powerOn=0, powerOff=0;
struct LSM6DSLSensor {
    LSM6DSLSensor(Bus*,int){++objects;} ~LSM6DSLSensor(){--objects;}
    int begin(){return beginFails;} int Enable_X(){return 0;} int Enable_Pedometer(){return 0;}
    void Enable_Single_Tap_Detection(int){} void Set_Tap_Threshold(int){}
    void Set_Tap_Shock_Time(int){} void Set_Tap_Quiet_Time(int){}
    void Enable_Wake_Up_Detection(int){} void Set_Wake_Up_Threshold(int){}
    int Set_G_ODR(float){return 0;} int Set_G_FS(float){return 0;}
    int Enable_G(){++powerOn;return 0;} void Disable_G(){++powerOff;}
};
void LSM6DSL_ACC_GYRO_W_STEP_DET_on_INT1(void*,int){}
void LSM6DSL_ACC_GYRO_W_WAKE_DUR(void*,int){}
void* heap_caps_malloc(size_t n,int){return malloc(n);} void heap_caps_free(void* p){free(p);}
namespace hal {namespace imu {
LSM6DSLSensor* s_imu=nullptr; bool s_pedoEnabled=false; uint8_t s_gyroUsers=0;
bool startGyroFifo(){return s_gyroUsers==1;} void stopGyroFifo(){}
"""
hal = "src/hal/hal_imu.cpp"
fixture += "\n".join(function(hal, sig) for sig in
                     ("bool init()", "bool isReady()", "bool acquireGyroscope()", "void releaseGyroscope()"))
fixture += "\n}}\nnamespace services {namespace motion {\nGyroFifo* fifoOwner=nullptr;\n"
for path, signature in [("motion.cpp","bool Gyroscope::open()"),("motion.cpp","void Gyroscope::close()"),
                        ("motion_fifo.cpp","bool GyroFifo::open()"),("motion_fifo.cpp","void GyroFifo::close()")]:
    fixture += function("src/services/" + path, signature) + "\n"
fixture += r"""
}}
int main(){
    using namespace hal::imu;
    services::motion::GyroFifo tennis,bowling;
    assert(!init()); assert(!tennis.open()); assert(!isReady() && objects==0);
    Wire1.online=true; beginFails=true;
    assert(!tennis.open()); assert(!isReady() && objects==0);
    beginFails=false;
    assert(tennis.open()); assert(tennis.open()); assert(!bowling.open());
    assert(objects==1 && powerOn==1);
    tennis.close(); assert(powerOff==1);
    assert(bowling.open()); bowling.close(); assert(powerOn==2 && powerOff==2);
}
"""
with tempfile.TemporaryDirectory() as d:
    source = Path(d)/"motion.cpp"
    source.write_text(fixture)
    exe = Path(d)/"test"
    subprocess.run(["c++","-std=c++17","-I"+str(ROOT/"include"),str(source),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS missed probe, driver failure, recovery at game entry, FIFO exclusion and release")
