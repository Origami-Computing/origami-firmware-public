#include "hal/hal_camera.h"

#include <Arduino.h>
#include <SD.h>
#include <Wire.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include "ESP32_OV5640_AF.h"
#include "Pins.h"
#include "hal/hal_bus.h"
#include "hal/hal_io.h"
#include "hal/hal_sd.h"
#include "core/telemetry.h"
#include "camera_event_diagnostics.h"

namespace hal {
namespace camera {

static bool s_initialized = false;
static uint16_t s_detectedPid = 0;
bool supportsAutofocus() { return s_detectedPid == 0x5640; }
static camera_fb_t* s_lastFb = nullptr;
static constexpr TickType_t kI2cLockTimeout = pdMS_TO_TICKS(600);

static void restartAccessoryBus() {
  Wire1.end();
  delay(20);
  Wire1.begin(ACC_SDA_PIN, ACC_SCL_PIN);
  Wire1.setClock(100000);
  Wire1.setTimeOut(50);
}

void deinit();

// Forward decl — internal-only helper, not exported through the header (see header rationale).
static bool applyTuningInternal(const Tuning& t);

struct StartupFailure {
  const char* stage;
  esp_err_t code;
  StartupFailure(const char* stage_ = "none", esp_err_t code_ = ESP_OK)
      : stage(stage_), code(code_) {}
};

// One hardware attempt. Public init() owns cleanup, retry, and Fleet logging.
// Never emit durable telemetry while holding I2C: it can acquire the SD bus.
static bool initAttempt(int frameSize, int pixelFormat, const Tuning* tuning,
                        int jpegQualityOverride, bool singleBuffer,
                        StartupFailure& failure) {
  if (!hal::bus::lockI2c(kI2cLockTimeout)) {
    failure = {"i2c-busy", ESP_ERR_TIMEOUT};
    Serial.println("[camera] Init failed: I2C bus busy");
    return false;
  }
  
  // Power sequence per OV5640 datasheet
  hal::io::enableRegulators(false, false);
  hal::io::setCameraPwdn(true);    // HIGH = powered down
  hal::io::setCameraReset(false);  // LOW = in reset
  delay(10);
  
  // Bring up analog 2.8 V before digital 1.8 V, then release PWDN/reset.
  hal::io::enableRegulators(true, false);
  delay(10);
  hal::io::enableRegulators(true, true);
  delay(10);
  
  // Release power-down
  hal::io::setCameraPwdn(false);
  delay(10);
  
  // Release reset
  hal::io::setCameraReset(true);
  delay(100);
  hal::io::dumpCameraPinState("released");

  // A powered OV5640 must ACK at 0x3C before the camera driver probes it.
  Wire1.beginTransmission(0x3C);
  const uint8_t cameraAck = Wire1.endTransmission(true);
  Serial.printf("[camera] accessory SCCB preflight 0x3C ack=%u lines=%d/%d\n",
                cameraAck, digitalRead(ACC_SDA_PIN), digitalRead(ACC_SCL_PIN));
  if (cameraAck != 0) {
    // Do not enter esp_camera_init() when the module is absent or its clock/
    // rails are unhealthy. The driver otherwise performs a much broader probe
    // and teardown sequence on the shared accessory controller, which can make
    // a missing camera look like an unrelated IMU/audio crash.
    failure = {"sensor-ack", (esp_err_t)cameraAck};
    Serial.println("[camera] Init failed: OV5640 did not ACK preflight");
    hal::io::setCameraPwdn(true);
    hal::io::setCameraReset(false);
    hal::io::enableRegulators(true, true);
    restartAccessoryBus();
    hal::bus::unlockI2c();
    return false;
  }

  framesize_t fsize = (frameSize >= 0) ? (framesize_t)frameSize : FRAMESIZE_QXGA;
  pixformat_t pfmt  = (pixelFormat >= 0) ? (pixformat_t)pixelFormat : PIXFORMAT_JPEG;

  // ---------------------------------------------------------------------------------------------
  // Memory plan — fb_count tuned to fit the per-framebuffer cost in available PSRAM.
  //
  // esp_camera (cam_hal.c) sizes the JPEG framebuffer as `width × height / 5` bytes — JPEG
  // quality affects the COMPRESSED OUTPUT FILE SIZE, NOT the allocation. So the only knobs we
  // have for memory pressure are:
  //   1) `fb_count` — number of framebuffers (each `width × height / 5` bytes contiguous).
  //   2) `fb_location` — PSRAM keeps internal DRAM free for the DMA scanline buffer.
  //
  // For high-res JPEG we always go fb_count=1; for VGA/HD we use 2 only if there's >= 2× the
  // single-fb cost free, so the second buffer doesn't push the total past the contig ceiling.
  //
  // Per-framebuffer sizes (KB) at fb_count=1: QSXGA≈960, FHD≈410, UXGA≈380, HD≈180, VGA≈60.
  // ---------------------------------------------------------------------------------------------
  camera_fb_location_t fbloc      = CAMERA_FB_IN_DRAM;
  int                  fbcount    = 1;
  int                  jpegQ      = 12;   // quality only affects output file size, not buffer
  size_t               freeContig = 0;
  if (psramFound()) {
    fbloc      = CAMERA_FB_IN_PSRAM;
    freeContig = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

#if defined(ORIGAMI_RGB_PREVIEW_TRIAL)
    if (pfmt==PIXFORMAT_RGB565 && fsize==FRAMESIZE_QVGA && freeContig>=400u*1024u) fbcount=2;
#endif
    if (pfmt == PIXFORMAT_JPEG) {
      if (fsize >= FRAMESIZE_UXGA) {
        // High-res: always single buffer. Quality stays at 12 — output JPEG file size scales with
        // quality but the alloc doesn't, so no point starving the file size.
        fbcount = 1;
        jpegQ   = 12;
      } else if (fsize >= FRAMESIZE_HD) {
        fbcount = (freeContig >= 500u * 1024u) ? 2 : 1;
        jpegQ   = 14;
      } else {
        fbcount = (freeContig >= 200u * 1024u) ? 2 : 1;
        // VGA/QVGA path is dominated by the wifi_setup QR scanner. quirc
        // binarises module edges and is sensitive to JPEG ringing/
        // quantisation, so we spend extra bytes for cleaner edges. jpegQ=6
        // roughly doubles file size vs 12 (~12–15 KB at QVGA, ~40–60 KB at
        // VGA — trivial against 1.4 MB PSRAM) and visibly tightens the
        // module boundaries in the decoded gray buffer. Matters even more at
        // VGA where wifi_setup_app crops the central 320×240 for scanning,
        // so ringing in that region directly costs detection reliability.
        jpegQ   = 6;
      }
    }
  }

  // A recovery attempt trades throughput for lower framebuffer pressure without
  // silently changing the requested resolution or JPEG quality.
  if (singleBuffer) fbcount = 1;

  if (pfmt == PIXFORMAT_JPEG && jpegQualityOverride >= 0) {
    jpegQ = constrain(jpegQualityOverride, 0, 63);
  }

  // XCLK comes from Y1, the board's external ASDMB-24.00MHz oscillator; the ESP
  // must not generate another clock onto that net. Although pin_xclk is -1,
  // esp32-camera still uses xclk_freq_hz to configure ESP32-S3 LCD_CAM timing
  // and to calculate the OV5640 sensor PLL. Describing this as the old 10 MHz
  // value makes both sides calculate against the wrong clock and can make
  // camera init/capture unstable.
  camera_config_t config = {
    .pin_pwdn = -1,              // rev4: controlled by expander (IOX_CAMERA_PWDN)
    .pin_reset = -1,             // rev4: controlled by expander (IOX_CAMERA_RESET)
    .pin_xclk = CAMERA_XCLK_PIN, // rev4: -1 (external 24 MHz oscillator)
    // Wire1 already owns accessory I2C. Telling esp32-camera to use controller
    // 1 avoids uninstalling/reinstalling the bus underneath the IMU and codec.
    .pin_sccb_sda = -1,
    .pin_sccb_scl = -1,
    .pin_d7 = CAMERA_Y9_PIN,
    .pin_d6 = CAMERA_Y8_PIN,
    .pin_d5 = CAMERA_Y7_PIN,
    .pin_d4 = CAMERA_Y6_PIN,
    .pin_d3 = CAMERA_Y5_PIN,
    .pin_d2 = CAMERA_Y4_PIN,
    .pin_d1 = CAMERA_Y3_PIN,
    .pin_d0 = CAMERA_Y2_PIN,
    .pin_vsync = CAMERA_VSYNC_PIN,
    .pin_href = CAMERA_HREF_PIN,
    .pin_pclk = CAMERA_PCLK_PIN,
    .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = pfmt,
    .frame_size = fsize,
    .jpeg_quality = jpegQ,
    .fb_count = fbcount,
    .fb_location = fbloc,
    .grab_mode = (fsize >= FRAMESIZE_UXGA) ? CAMERA_GRAB_WHEN_EMPTY : CAMERA_GRAB_LATEST,
    .sccb_i2c_port = 1,
  };

  Serial.printf("[camera] config fsize=%d fbcount=%d jpegQ=%d psram=%d freeContig=%u KB freeTotal=%u KB\n",
                (int)fsize, fbcount, jpegQ, psramFound() ? 1 : 0,
                (unsigned)(freeContig / 1024u),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    failure = {"driver-init", err};
    Serial.printf("[camera] Init failed: %d\n", err);
    // Also cover partial driver initialization. Deinit is safe when the driver
    // already cleaned itself up; never retry over a remaining allocation.
    esp_camera_deinit();
#if defined(ORIGAMI_CAMERA_EVENT_DIAGNOSTICS)
    cameraEventFinish();
#endif
    // SCCB borrows the already-installed Wire1 controller, so failure must not
    // tear down the shared IMU/codec bus.
    hal::io::setCameraPwdn(true);
    hal::io::setCameraReset(false);
    // Keep both camera rails up: on this rev, removing them loads the shared
    // accessory I2C lines low through the camera/level-shifter domain.
    hal::io::enableRegulators(true, true);
    restartAccessoryBus();
    hal::bus::unlockI2c();
    return false;
  }

  /*
   * Sensor ISP defaults (natural look). Previous defaults came from an OV2640 RNT tutorial and
   * were overdriving the sensor: +1 brightness, +2 contrast, manual aec_value=400, manual
   * agc_gain=2, gainceiling 4X. That combination forced exposure/gain off-auto and crushed AWB,
   * producing the "uncalibrated / not real" tint. Tuning struct (default-constructed) below uses
   * neutral values; callers (camera_menu_app) can pass their own struct in to override.
   *
   * SCCB borrows the already-installed accessory-I²C controller, so this
   * configuration and later Wire1 autofocus operations share the same bus.
   */
  Tuning effective = tuning ? *tuning : Tuning{};
  bool tuningOk = applyTuningInternal(effective);
  sensor_t* detectedSensor = esp_camera_sensor_get();
  s_detectedPid = detectedSensor ? detectedSensor->id.PID : 0;
#if defined(ORIGAMI_VIDEO_HD_CROP_TRIAL)
  // The stock HD preset scales a 1920x1080 sensor window and retains its
  // slower readout timing. This isolated trial uses centered, unbinned native
  // 1280x720 pixels, retaining the existing PLL (no clock increase). The smaller
  // field of view is deliberate. HTS*VTS targets 30 Hz at the driver's 60 MHz
  // system clock with this board's external 24 MHz input; measure actual FPS.
  if (fsize == FRAMESIZE_HD && pfmt == PIXFORMAT_JPEG && detectedSensor &&
      detectedSensor->id.PID == 0x3660) {
    const int result=detectedSensor->set_res_raw ? detectedSensor->set_res_raw(
        detectedSensor,384,408,1695,1139,16,6,1604,1247,1280,720,false,false) : -1;
    tuningOk=tuningOk && result==0;
    Serial.printf("[camera-hd-crop] result=%d window=1280x720 hts=1604 vts=1247\n",result);
  }
#endif
  Serial.printf("[camera] Sensor ISP configured (PID=0x%04X) — natural defaults\n",
                detectedSensor ? (unsigned)detectedSensor->id.PID : 0u);

  s_initialized = true;
#if defined(ORIGAMI_CAMERA_EVENT_DIAGNOSTICS)
  // Read only during initialization under the accessory I2C lock, before a
  // caller can acquire frames. OV3660 datasheet v1.3 DVP/VFIFO controls let
  // pulse-width observations be compared with actual sensor configuration.
  if (detectedSensor && detectedSensor->id.PID == 0x3660 && detectedSensor->get_reg) {
    const uint16_t registers[] = {0x4709, 0x470a, 0x470b, 0x4713, 0x471d,
                                  0x4740, 0x4600, 0x4602, 0x4603, 0x460c};
    for (uint16_t reg : registers) {
      const int value = detectedSensor->get_reg(detectedSensor, reg, 0xff);
      Serial.printf("[camera-dvp-probe] reg=0x%04X value=%d\n", unsigned(reg), value);
    }
  }
  cameraEventBegin();
#endif
  Serial.println("[camera] Initialized");
  hal::bus::unlockI2c();
  if (!tuningOk) {
    failure = {"sensor-tuning", ESP_FAIL};
    return false;
  }

  // A configured driver is not proof that DMA/cam_task is producing frames.
  // Probe once here so every caller receives the same ready-to-capture contract.
  camera_fb_t* first = esp_camera_fb_get();
  const bool ready = first && first->buf && first->len > 0;
  if (first) esp_camera_fb_return(first);
  if (!ready) failure = {"first-frame", ESP_ERR_TIMEOUT};
  return ready;
}

static void logStartupMemory(unsigned attempt) {
  // Keep each message within the telemetry facade's 95-byte message limit.
  LOGI("camera", "try=%u dma=%u max=%u psram=%u max=%u", attempt,
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static char s_initError[80]="Camera could not open";
const char* lastInitError(){return s_initError;}
bool init(int frameSize, int pixelFormat, const Tuning* tuning, int jpegQualityOverride) {
  const uint32_t started = millis();
  LOGI("camera", "init size=%d format=%d quality=%d", frameSize, pixelFormat, jpegQualityOverride);
  for (unsigned attempt = 1; attempt <= 2; ++attempt) {
    if (s_initialized) deinit();
    if (s_initialized) {
      snprintf(s_initError,sizeof(s_initError),"Previous camera session is still busy");
      LOGE("camera", "init blocked: previous session still owns resources");
      return false;
    }
    // Self-deleting network tasks reclaim their stacks on the idle task. Give
    // both cores a scheduling opportunity before allocating camera DMA memory.
    s_detectedPid = 0;
    delay(20);
    logStartupMemory(attempt);
    StartupFailure failure;
    if (initAttempt(frameSize, pixelFormat, tuning, jpegQualityOverride,
                    attempt > 1, failure)) {
      LOGI("camera", "ready try=%u elapsed=%lu ms", attempt,
           (unsigned long)(millis() - started));
      return true;
    }
    snprintf(s_initError,sizeof(s_initError),"%s (0x%x)%s",failure.stage,(unsigned)failure.code,failure.code==ESP_ERR_NO_MEM?": not enough memory":"");
    // Capture failure-time DMA headroom before releasing the driver. Unlike
    // the routine startup sample, this warning gets best-effort SD journaling.
    const unsigned dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const unsigned dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    // First-frame/tuning failures have a live driver; release it before retry
    // or returning failure. Driver/preflight failures already cleaned up.
    if (s_initialized) deinit();
    LOGW("camera", "init failed try=%u stage=%s code=0x%x", attempt,
         failure.stage, (unsigned)failure.code);
    LOGW("camera", "failure memory try=%u dma=%u max=%u", attempt, dmaFree, dmaLargest);
  }
  LOGE("camera", "init exhausted after 2 attempts elapsed=%lu ms",
       (unsigned long)(millis() - started));
  return false;
}

void deinit() {
  if (!hal::bus::lockI2c(kI2cLockTimeout)) {
    LOGW("camera", "deinit blocked: I2C bus busy");
    return;
  }
  returnFrame();
  if (s_initialized) {
    esp_camera_deinit();
#if defined(ORIGAMI_CAMERA_EVENT_DIAGNOSTICS)
    cameraEventFinish();
#endif
    s_initialized = false;
  }
  
  // Power down camera
  hal::io::setCameraPwdn(true);
  hal::io::setCameraReset(false);
  // Keep the rails up so the unpowered camera domain cannot clamp Wire1.
  hal::io::enableRegulators(true, true);
  restartAccessoryBus();
  
  Serial.println("[camera] Deinitialized");
  hal::bus::unlockI2c();
  LOGD("camera", "released driver and frame buffers");
}

bool getFrame(CameraFrame* out) {
  if (!out || !s_initialized || s_lastFb) return false;
  s_lastFb = esp_camera_fb_get();
  if (!s_lastFb) return false;
#if defined(ORIGAMI_CAMERA_EVENT_DIAGNOSTICS)
  if (s_lastFb->format == PIXFORMAT_JPEG)
    cameraEventObserve(s_lastFb->buf, s_lastFb->len);
#endif
  out->buf = s_lastFb->buf;
  out->len = s_lastFb->len;
  out->width = s_lastFb->width;
  out->height = s_lastFb->height;
  out->format = s_lastFb->format;
  out->dmaStartUs = (uint64_t)s_lastFb->timestamp.tv_sec * 1000000ULL +
                   (uint64_t)s_lastFb->timestamp.tv_usec;
  return true;
}

void returnFrame() {
  if (s_lastFb) {
    esp_camera_fb_return(s_lastFb);
    s_lastFb = nullptr;
  }
}

bool captureToFile(const char* path) {
  if (!s_initialized || !path) return false;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    LOGW("camera", "captureToFile: frame unavailable");
    return false;
  }
  const size_t bytes = fb->len;
  File f = SD.open(path, FILE_WRITE);
  bool ok = f && (f.write(fb->buf, bytes) == bytes);
  if (f) f.close();
  esp_camera_fb_return(fb);
  if (!ok) {
    SD.remove(path);
    LOGW("camera", "captureToFile: SD write failed");
  } else {
    LOGI("camera", "captureToFile: saved %u bytes", (unsigned)bytes);
  }
  return ok;
}

size_t availablePsramKb() {
  if (!psramFound()) return 0u;
  return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u;
}

bool captureToSd() {
  if (!s_initialized) {
    LOGW("camera", "capture requested without initialized camera");
    return false;
  }
  
  if (!hal::sd::mounted()) {
    LOGW("camera", "capture failed: SD not mounted");
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    LOGW("camera", "capture failed: frame unavailable");
    return false;
  }
  
  size_t imgSize = fb->len;
  uint8_t* imgBuf = (uint8_t*)malloc(imgSize);
  if (!imgBuf) {
    esp_camera_fb_return(fb);
    LOGW("camera", "capture failed: JPEG copy allocation");
    return false;
  }
  memcpy(imgBuf, fb->buf, imgSize);
  esp_camera_fb_return(fb);

  char fname[32];
  snprintf(fname, sizeof(fname), "/cam%lu.jpg", (unsigned long)millis());

  File f = SD.open(fname, FILE_WRITE);
  if (!f) {
    free(imgBuf);
    LOGW("camera", "capture failed: cannot open SD file");
    return false;
  }
  
  size_t written = f.write(imgBuf, imgSize);
  f.close();
  free(imgBuf);

  Serial.printf("[camera] Saved %s (%d bytes)\n", fname, written);
  return written == imgSize;
}

bool captureJpeg(uint8_t** outBuf, size_t* outLen) {
  if (!s_initialized) {
    LOGW("camera", "capture requested without initialized camera");
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    LOGW("camera", "capture failed: frame unavailable");
    return false;
  }

  *outLen = fb->len;
  *outBuf = (uint8_t*)malloc(fb->len);
  if (!*outBuf) {
    esp_camera_fb_return(fb);
    LOGW("camera", "capture failed: JPEG copy allocation");
    return false;
  }
  memcpy(*outBuf, fb->buf, fb->len);
  esp_camera_fb_return(fb);

  Serial.printf("[camera] Captured JPEG: %u bytes\n", (unsigned)*outLen);
  return true;
}

bool initialized() {
  return s_initialized;
}

static bool applyTuningInternal(const Tuning& t) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;

  // Clamp helpers — esp_camera silently rejects out-of-range values, but the sensor sometimes ends
  // up half-applied (one register set, the next not). Clamping here keeps the state consistent.
  auto clamp_pm2 = [](int8_t v) -> int8_t { return v < -2 ? -2 : (v > 2 ? 2 : v); };
  auto clamp_pm3 = [](int8_t v) -> int8_t { return v < -3 ? -3 : (v > 3 ? 3 : v); };
  auto clamp_wb  = [](uint8_t v) -> uint8_t { return v > 4 ? 4 : v; };

  s->set_special_effect(s, 0);
  s->set_colorbar(s, 0);

  s->set_brightness(s, clamp_pm2(t.brightness));
  s->set_contrast(s,   clamp_pm2(t.contrast));
  s->set_saturation(s, clamp_pm2(t.saturation));
  s->set_sharpness(s,  clamp_pm3(t.sharpness));

  s->set_whitebal(s, t.whitebal ? 1 : 0);
  s->set_awb_gain(s, t.awb_gain ? 1 : 0);
  s->set_wb_mode(s, clamp_wb(t.wb_mode));

  s->set_exposure_ctrl(s, t.exposure_ctrl ? 1 : 0);
  const bool nightMode=s->id.PID!=0x3660 || t.allowNightMode;
  const int nightResult=s->set_aec2(s,nightMode ? 1 : 0);
  if (!nightMode && nightResult!=0) return false;
  Serial.printf("[camera] night_mode=%d result=%d\n",nightMode ? 1 : 0,nightResult);
  s->set_ae_level(s, clamp_pm2(t.ae_level));
  // No set_aec_value / set_agc_gain calls — leaving auto in charge gives realistic exposure.

  s->set_gain_ctrl(s, t.gain_ctrl ? 1 : 0);
  static const gainceiling_t kCeil[] = {GAINCEILING_2X, GAINCEILING_4X, GAINCEILING_8X, GAINCEILING_16X};
  uint8_t gci = t.gainceiling_idx; if (gci >= 4) gci = 0;
  s->set_gainceiling(s, kCeil[gci]);

  s->set_bpc(s,     t.bpc ? 1 : 0);
  s->set_wpc(s,     t.wpc ? 1 : 0);
  s->set_raw_gma(s, t.raw_gma ? 1 : 0);
  s->set_lenc(s,    t.lenc ? 1 : 0);
  s->set_dcw(s,     t.dcw ? 1 : 0);

  s->set_hmirror(s, t.hmirror ? 1 : 0);
  s->set_vflip(s,   t.vflip ? 1 : 0);
  Serial.printf("[camera] orientation hmirror=%d vflip=%d\n",
                t.hmirror ? 1 : 0, t.vflip ? 1 : 0);

  // Load the OV5640 autofocus firmware while SCCB is still ours. After this
  // function returns init() leaves Wire1 installed for Arduino and we'd no longer be
  // able to talk to the sensor's MCU over the shared I2C bus.
  if (t.autofocus) {
    OV5640 af;
    const uint8_t afCidH = s->get_reg(s, 0x300A, 0xff);
    const uint8_t afCidL = s->get_reg(s, 0x300B, 0xff);
    Serial.printf("[camera] AF preflight driverPid=0x%04X regs=%02X%02X\n",
                  (unsigned)s->id.PID, (unsigned)afCidH, (unsigned)afCidL);
    if (!af.start(s)) {
      Serial.println("[camera] AF: sensor is not OV5640, skipping");
    } else {
      const uint32_t t0 = millis();
      const uint8_t  fi = af.focusInit();
      if (fi != 0) {
        Serial.printf("[camera] AF: focusInit failed (rc=%u, %lu ms) — staying fixed-focus\n",
                      (unsigned)fi, (unsigned long)(millis() - t0));
      } else if (t.singleShotAf) {
        // Caller wants a one-time sweep + lock (stationary close-up scanning).
        // Skip continuous mode entirely: no hunt-induced blur once we return.
        const uint8_t sh = af.singleShotAutoFocus();
        if (sh == 0) {
          Serial.printf("[camera] AF: single-shot locked (%lu ms)\n",
                        (unsigned long)(millis() - t0));
        } else {
          Serial.printf("[camera] AF: single-shot failed (rc=%u, %lu ms) — fixed focus\n",
                        (unsigned)sh, (unsigned long)(millis() - t0));
        }
      } else {
        const uint8_t cm = af.autoFocusMode();
        if (cm == 0) {
          Serial.printf("[camera] AF: continuous mode active (%lu ms)\n",
                        (unsigned long)(millis() - t0));
        } else {
          // Continuous mode's "relax motor" handshake didn't ACK in 5 s. Fall
          // back to a single shot: the firmware will sweep the VCM through
          // its full range, pick the highest-contrast position, and park
          // there. That gives us one good fixed-focus snapshot tuned to
          // whatever was in front of the camera at boot — far better than
          // the lens sitting wherever it powered up.
          const uint8_t sh = af.singleShotAutoFocus();
          if (sh == 0) {
            Serial.printf("[camera] AF: continuous failed (rc=%u), single-shot locked (%lu ms)\n",
                          (unsigned)cm, (unsigned long)(millis() - t0));
          } else {
            Serial.printf("[camera] AF: both modes failed (cont=%u, single=%u, %lu ms) — fixed focus\n",
                          (unsigned)cm, (unsigned)sh, (unsigned long)(millis() - t0));
          }
        }
      }
    }
  }

  return true;
}

// ----- On-demand autofocus over Wire1 -----------------------------------------------
//
// After init() the I2C controller is owned by Arduino, so we can't use
// sensor->set_reg / get_reg through esp_camera's SCCB any more (it tries to
// reinstall the I2C driver and times out). But OV5640 SCCB is just standard
// I2C at slave address 0x3C with 16-bit register addresses, so we can drive
// the same registers from Wire1 directly. The AF firmware is already running
// inside the sensor — we just nudge its command register.
namespace {
constexpr uint8_t  kOv5640I2cAddr  = 0x3C;
constexpr uint16_t kRegChipIdHigh  = 0x300A;
constexpr uint16_t kRegChipIdLow   = 0x300B;
constexpr uint16_t kRegCmdMain     = 0x3022;
constexpr uint16_t kRegCmdAck      = 0x3023;
constexpr uint16_t kRegFwStatus    = 0x3029;
constexpr uint8_t  kAfTrigSingle   = 0x03;
constexpr uint8_t  kFwStatusFocusing = 0x00;  // upstream uses "0x00..0x0F or 0x80..0x8F == focusing"
constexpr uint8_t  kFwStatusFocused = 0x10;

bool ovWriteReg(uint16_t reg, uint8_t val) {
  Wire1.beginTransmission(kOv5640I2cAddr);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)(reg & 0xFF));
  Wire1.write(val);
  return Wire1.endTransmission() == 0;
}

bool ovReadReg(uint16_t reg, uint8_t* outVal) {
  Wire1.beginTransmission(kOv5640I2cAddr);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)(reg & 0xFF));
  if (Wire1.endTransmission(false) != 0) return false;  // repeated start
  if (Wire1.requestFrom((int)kOv5640I2cAddr, 1) != 1) return false;
  *outVal = Wire1.read();
  return true;
}
}  // namespace

bool triggerAutofocus(uint32_t timeoutMs) {
  if (!s_initialized) return false;
  if (!hal::bus::lockI2c(kI2cLockTimeout)) {
    Serial.println("[camera] AF refocus: I2C bus busy");
    return false;
  }

  // -- Diagnostic --------------------------------------------------------------
  // Read OV5640 CHIP_ID over Wire1. If we get 0x56/0x40 we KNOW Wire1→OV5640
  // comms is fully functional and any AF flakiness is purely firmware state.
  // If chip ID reads garbage (e.g. 0xFF/0xFF), Wire1 can't actually talk to
  // the sensor despite endTransmission returning ACK — usually that's a sign
  // the accessory I2C bus is not reaching the sensor.
  uint8_t cidH = 0xFF, cidL = 0xFF;
  const bool cidOk = ovReadReg(kRegChipIdHigh, &cidH) && ovReadReg(kRegChipIdLow, &cidL);
  uint8_t fwBefore = 0xFF;
  (void)ovReadReg(kRegFwStatus, &fwBefore);
  Serial.printf("[camera] AF refocus: chipId=%02X%02X (%s) fwStatus(before)=0x%02X\n",
                (unsigned)cidH, (unsigned)cidL,
                (cidOk && cidH == 0x56 && cidL == 0x40) ? "OK" : "MISMATCH",
                (unsigned)fwBefore);

  bool ok = false;

  // ACK=1 tells the firmware "command ready", MAIN=0x03 fires a single AF cycle.
  if (ovWriteReg(kRegCmdAck, 0x01) && ovWriteReg(kRegCmdMain, kAfTrigSingle)) {
    // Poll CMD_ACK back to 0. The OV5640 firmware only clears ACK once the
    // full focus sweep is done — that's a real physical motion of the VCM
    // lens through its range, typically 1.5–3 s — so the budget MUST be in
    // the multi-second range or we time out mid-sweep with the lens parked
    // at some random intermediate position.
    const uint32_t deadline = millis() + timeoutMs;
    const uint32_t startMs  = millis();
    bool acked = false;
    uint8_t lastAck = 0xFF;
    uint32_t reads = 0;
    while (millis() < deadline) {
      uint8_t a = 0xFF;
      if (!ovReadReg(kRegCmdAck, &a)) {
        Serial.println("[camera] AF refocus: CMD_ACK read failed mid-poll");
        break;
      }
      lastAck = a;
      reads++;
      if (a == 0x00) { acked = true; break; }
      delay(5);
    }
    uint8_t fwAfter = 0xFF;
    (void)ovReadReg(kRegFwStatus, &fwAfter);
    Serial.printf("[camera] AF refocus: %s in %lu ms (reads=%lu, lastAck=0x%02X, fw(after)=0x%02X)\n",
                  acked ? "ACK CLEARED" : "ack TIMEOUT",
                  (unsigned long)(millis() - startMs),
                  (unsigned long)reads, (unsigned)lastAck, (unsigned)fwAfter);
    if (acked) {
      delay(60);
      ok = true;
    }
  } else {
    Serial.println("[camera] AF refocus: SCCB write failed (sensor not reachable)");
  }

  hal::bus::unlockI2c();
  return ok;
}

void settleAutoExposure(uint32_t millisBudget) {
  if (!s_initialized) return;
  const uint32_t deadline = millis() + millisBudget;
  // OV5640 AE/AWB needs ~5–10 frames to converge after a power-up or framesize change. Pulling
  // them off the bus during this window prevents the first user-facing capture from showing the
  // sensor's "first guess" (often green/blue tint).
  while (millis() < deadline) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(20);
  }
}

}  // namespace camera
}  // namespace hal
