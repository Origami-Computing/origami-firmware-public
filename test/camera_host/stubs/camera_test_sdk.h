#pragma once
// Host doubles for compiling the real camera HAL and injecting startup faults.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <string>
#include <vector>
using TickType_t = unsigned;
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_TIMEOUT=0x107, ESP_ERR_NO_MEM=0x101;
#define pdMS_TO_TICKS(x) (x)
#define constrain(x,a,b) std::min(std::max((x),(a)),(b))
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define ACC_SDA_PIN 1
#define ACC_SCL_PIN 2
#define CAMERA_XCLK_PIN -1
#define CAMERA_XCLK_FREQ_HZ 24000000
#define CAMERA_Y9_PIN 0
#define CAMERA_Y8_PIN 0
#define CAMERA_Y7_PIN 0
#define CAMERA_Y6_PIN 0
#define CAMERA_Y5_PIN 0
#define CAMERA_Y4_PIN 0
#define CAMERA_Y3_PIN 0
#define CAMERA_Y2_PIN 0
#define CAMERA_VSYNC_PIN 0
#define CAMERA_HREF_PIN 0
#define CAMERA_PCLK_PIN 0
#define FILE_WRITE 1
using framesize_t=int;
using pixformat_t=int;
using camera_fb_location_t=int;
using gainceiling_t=int;
constexpr int FRAMESIZE_QVGA=5, FRAMESIZE_VGA=8, FRAMESIZE_HD=11,
              FRAMESIZE_UXGA=13, FRAMESIZE_QXGA=15, PIXFORMAT_JPEG=4,
              CAMERA_FB_IN_DRAM=0, CAMERA_FB_IN_PSRAM=1,
              CAMERA_GRAB_WHEN_EMPTY=0, CAMERA_GRAB_LATEST=1,
              LEDC_TIMER_0=0, LEDC_CHANNEL_0=0,
              GAINCEILING_2X=0, GAINCEILING_4X=1, GAINCEILING_8X=2, GAINCEILING_16X=3;
struct camera_config_t {
  int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl;
  int pin_d7,pin_d6,pin_d5,pin_d4,pin_d3,pin_d2,pin_d1,pin_d0;
  int pin_vsync,pin_href,pin_pclk,xclk_freq_hz,ledc_timer,ledc_channel;
  int pixel_format,frame_size,jpeg_quality,fb_count,fb_location,grab_mode,sccb_i2c_port;
};
struct camera_fb_t {
  uint8_t* buf; size_t len; unsigned width,height; int format; timeval timestamp;
};
struct sensor_t {
  struct { unsigned PID=0x5640; } id;
#define SETTER(name) int name(sensor_t*, int) { return 0; }
  SETTER(set_special_effect) SETTER(set_colorbar) SETTER(set_brightness)
  SETTER(set_contrast) SETTER(set_saturation) SETTER(set_sharpness)
  SETTER(set_whitebal) SETTER(set_awb_gain) SETTER(set_wb_mode)
  SETTER(set_exposure_ctrl) SETTER(set_ae_level)
  int nightApplied=-1,nightFailure=0;
  int set_aec2(sensor_t*,int value){nightApplied=value;return nightFailure;}
  SETTER(set_gain_ctrl) SETTER(set_gainceiling) SETTER(set_bpc) SETTER(set_wpc)
  SETTER(set_raw_gma) SETTER(set_lenc) SETTER(set_dcw) SETTER(set_hmirror) SETTER(set_vflip)
#undef SETTER
  int get_reg(sensor_t*,int,int) { return 0; }
};
struct FakeState {
  unsigned now=0;
  bool driver=false, borrowed=false, i2c=false, pwdn=true, reset=false;
  int driverFailures=0, frameFailures=0, ackFailures=0, lockFailures=0;
  int deinitializations=0, returned=0, busRestarts=0;
  size_t dmaLargest=24000;
  std::vector<camera_config_t> configs;
  std::vector<std::string> logs;
};
extern FakeState fake;
inline unsigned millis() { return fake.now; }
inline void delay(unsigned ms) { fake.now+=ms; }
inline int digitalRead(int) { return 1; }
inline bool psramFound() { return true; }
inline size_t heap_caps_get_free_size(int cap) { return cap==MALLOC_CAP_DMA ? 48000 : 7000000; }
inline size_t heap_caps_get_largest_free_block(int cap) { return cap==MALLOC_CAP_DMA ? fake.dmaLargest : 6000000; }
struct SerialType {
  template<class... T> void printf(const char*, T...) {}
  void println(const char*) {}
};
extern SerialType Serial;
struct WireType {
  void end() { ++fake.busRestarts; }
  void begin(int,int) {}
  void setClock(int) {}
  void setTimeOut(int) {}
  void beginTransmission(int) {}
  int endTransmission(bool=true) {
    if (fake.ackFailures>0) { --fake.ackFailures; return 2; }
    return 0;
  }
  void write(uint8_t) {}
  int requestFrom(int,int) { return 1; }
  int read() { return 0; }
};
extern WireType Wire1;
struct File {
  explicit operator bool() const { return true; }
  size_t write(const uint8_t*, size_t n) { return n; }
  void close() {}
};
struct SDType { File open(const char*,int) { return {}; } void remove(const char*) {} };
extern SDType SD;
struct OV5640 {
  bool start(sensor_t*) { return true; }
  int focusInit() { return 0; }
  int singleShotAutoFocus() { return 0; }
  int autoFocusMode() { return 0; }
};
namespace hal {
namespace bus {
inline bool lockI2c(TickType_t) {
  if (fake.lockFailures>0) { --fake.lockFailures; return false; }
  assert(!fake.i2c); fake.i2c=true; return true;
}
inline void unlockI2c() { assert(fake.i2c); fake.i2c=false; }
}
namespace io {
inline void enableRegulators(bool,bool) { assert(fake.i2c); }
inline void setCameraPwdn(bool value) { assert(fake.i2c); fake.pwdn=value; }
inline void setCameraReset(bool value) { assert(fake.i2c); fake.reset=value; }
inline void dumpCameraPinState(const char*) {}
}
namespace sd { inline bool mounted() { return true; } }
}
esp_err_t esp_camera_init(const camera_config_t*);
esp_err_t esp_camera_deinit();
camera_fb_t* esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t*);
sensor_t* esp_camera_sensor_get();
