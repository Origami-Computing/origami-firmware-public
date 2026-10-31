#include "camera_test_sdk.h"
#include "hal/hal_camera.h"
#include "core/telemetry.h"
#include <cstdarg>

FakeState fake;
SerialType Serial;
WireType Wire1;
SDType SD;
static sensor_t sensor;
static uint8_t jpeg[]={0xff,0xd8,0xff,0xd9};
static camera_fb_t frame={jpeg,sizeof(jpeg),640,480,PIXFORMAT_JPEG,{0,1}};

esp_err_t esp_camera_init(const camera_config_t* config) {
  assert(!fake.driver); // No allocation may survive into a second init.
  fake.configs.push_back(*config);
  fake.driver=true; // Simulate partial allocation even when init fails.
  if (fake.driverFailures>0) { --fake.driverFailures; return ESP_ERR_NO_MEM; }
  return ESP_OK;
}
esp_err_t esp_camera_deinit() {
  assert(!fake.borrowed); // Return all checked-out frames before teardown.
  fake.driver=false;
  ++fake.deinitializations;
  return ESP_OK;
}
camera_fb_t* esp_camera_fb_get() {
  assert(fake.driver && !fake.borrowed);
  if (fake.frameFailures>0) { --fake.frameFailures; return nullptr; }
  fake.borrowed=true;
  return &frame;
}
void esp_camera_fb_return(camera_fb_t* fb) {
  assert(fb==&frame && fake.borrowed);
  fake.borrowed=false;
  ++fake.returned;
}
sensor_t* esp_camera_sensor_get() { return fake.driver ? &sensor : nullptr; }
namespace core { namespace telemetry {
void log(Level, const char*, const char* fmt, ...) {
  assert(!fake.i2c); // Remote warning journaling must never nest inside I2C.
  char msg[96];
  va_list args; va_start(args,fmt);
  const int len=vsnprintf(msg,sizeof(msg),fmt,args);
  va_end(args);
  assert(len>=0 && len<(int)sizeof(msg));
  fake.logs.emplace_back(msg);
}
}}
static bool hasLog(const char* needle) {
  for (const auto& line:fake.logs) if (line.find(needle)!=std::string::npos) return true;
  return false;
}
static void reset() {
  fake.lockFailures=0;
  hal::camera::deinit();
  assert(!hal::camera::initialized() && !fake.driver && !fake.borrowed && !fake.i2c);
  fake=FakeState{};
  sensor=sensor_t{};
}
static bool start() { return hal::camera::init(FRAMESIZE_VGA,PIXFORMAT_JPEG,nullptr,9); }
int main() {
  assert(!hal::camera::supportsAutofocus());
  // Normal startup verifies and returns a frame, then re-init returns a borrowed
  // application frame before releasing/replacing the old driver.
  assert(start());
  assert(fake.returned==1 && fake.configs.size()==1 && fake.configs[0].fb_count==2);
  hal::camera::CameraFrame out{};
  assert(hal::camera::getFrame(&out));
  assert(start());
  assert(fake.returned==3 && fake.deinitializations==1);
  reset();

  // Allocation failure must free partial driver state before one-buffer retry.
  fake.driverFailures=1;
  assert(start());
  assert(fake.configs.size()==2 && fake.configs[1].fb_count==1);
  assert(fake.configs[1].frame_size==FRAMESIZE_VGA && fake.configs[1].jpeg_quality==9);
  assert(fake.deinitializations==1 && hasLog("stage=driver-init") && hasLog("ready try=2"));
  reset();

  fake.driverFailures=3;
  assert(!start());
  assert(fake.configs.size()==2 && fake.deinitializations==2 && !fake.driver);
  assert(!hal::camera::initialized() && fake.pwdn && !fake.reset && !fake.i2c);
  assert(hasLog("exhausted") && hasLog("failure memory"));
  reset();

  // Driver success with no frames is failure, not a working camera.
  fake.frameFailures=1;
  assert(start());
  assert(fake.configs.size()==2 && fake.deinitializations==1 && hasLog("stage=first-frame"));
  reset();
  fake.frameFailures=3;
  assert(!start());
  assert(!fake.driver && !hal::camera::initialized() && fake.deinitializations==2);
  reset();

  // Missing sensor skips driver allocation, restores the shared bus, then retries.
  fake.ackFailures=1;
  assert(start());
  assert(fake.configs.size()==1 && fake.busRestarts==1 && hasLog("stage=sensor-ack"));
  reset();

  fake.lockFailures=2;
  assert(!start());
  assert(fake.configs.empty() && hasLog("stage=i2c-busy"));
  reset();

  // A blocked teardown must not start a second driver over the live session.
  assert(start());
  fake.lockFailures=1;
  assert(!start());
  assert(fake.configs.size()==1 && fake.driver && hal::camera::initialized());
  assert(hasLog("previous session still owns resources"));
  reset();
  // Video only disables the OV3660 night extension; default and other sensors
  // keep existing behavior. A failed requested change must unwind both retries.
  hal::camera::Tuning video;video.allowNightMode=false;
  sensor.id.PID=0x3660;
  assert(hal::camera::init(FRAMESIZE_VGA,PIXFORMAT_JPEG,&video,9));
  assert(sensor.nightApplied==0);reset();
  sensor.id.PID=0x3660;assert(start());assert(sensor.nightApplied==1);reset();
  assert(hal::camera::init(FRAMESIZE_VGA,PIXFORMAT_JPEG,&video,9));
  assert(sensor.nightApplied==1);reset();
  sensor.id.PID=0x3660;sensor.nightFailure=-1;
  assert(!hal::camera::init(FRAMESIZE_VGA,PIXFORMAT_JPEG,&video,9));
  assert(!fake.driver&&!hal::camera::initialized()&&hasLog("stage=sensor-tuning"));reset();
  sensor.id.PID=0x5640;assert(start());assert(hal::camera::supportsAutofocus());
  hal::camera::deinit();assert(hal::camera::supportsAutofocus());
  fake.ackFailures=2;assert(!start());assert(!hal::camera::supportsAutofocus());reset();
  sensor.id.PID=0x3660;assert(start());assert(!hal::camera::supportsAutofocus());
  hal::camera::deinit();assert(!hal::camera::supportsAutofocus());reset();
  puts("Passed camera startup/recovery, sensor-specific night mode and retained autofocus capability against production HAL.");
}
