#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Camera HAL - manages the board camera sensor (currently OV3660 on rev4).
// Frame size and format constants (match esp_camera values)
#define HAL_CAMERA_FRAMESIZE_QVGA  5
#define HAL_CAMERA_FRAMESIZE_VGA   8
#define HAL_CAMERA_PIXFORMAT_JPEG  4
// NOTE: Camera uses LCD_CAM peripheral with GDMA
// VideoRecording owns concurrent microphone RX under retained Camera admission.
// Other audio workflows still require their normal exclusive session policy.

namespace hal {
namespace camera {

// UI-task, cached capability; no sensor I/O. True when the latest init attempt
// identified the OV5640 supported by our AF driver. Retained through deinit so
// settings can query it with the camera off. False if no supported sensor was
// identified. This does not establish lens-motor presence or a successful lock.
bool supportsAutofocus();

// Frame data from getFrame(); valid until returnFrame() is called
struct CameraFrame {
  const uint8_t* buf;
  size_t len;
  uint16_t width;
  uint16_t height;
  int format;  // PIXFORMAT_JPEG etc.
  uint64_t dmaStartUs;  // Driver timestamp since boot; not optical exposure start.
};

/**
 * Camera ISP knobs that the UI / app layer is expected to drive. Defaults are tuned for natural,
 * neutral indoor colors — *not* the over-saturated/over-contrasted "low-light tweak" we shipped
 * before. Re-apply via applyTuning() any time the framesize is changed (some sensor settings reset
 * on resolution switch).
 */
struct Tuning {
  /** -2..+2 (0 = neutral). Anything > 0 tends to crush highlights. */
  int8_t brightness = 0;
  /** -2..+2 (0 = neutral). +2 produces the over-contrasted "punchy" look that loses skin tones. */
  int8_t contrast = 0;
  /** -2..+2 (0 = neutral). +1 if you want slightly punchier color without skewing AWB. */
  int8_t saturation = 0;
  /** -3..+3 sharpening (0 = neutral; sensor support varies). */
  int8_t sharpness = 0;
  /** -2..+2 auto-exposure target offset. 0 keeps AE honest. */
  int8_t ae_level = 0;
  /** 0=Auto, 1=Sunny, 2=Cloudy, 3=Office, 4=Home — feeds the sensor AWB preset table. */
  uint8_t wb_mode = 0;
  /** 0..3 esp_camera GAINCEILING_*X. 1 = 4X (a sane ceiling for indoor without amplifying noise). */
  uint8_t gainceiling_idx = 1;
  /**
   * Mirror/flip the sensor before the JPEG is encoded.
   *
   * Normalize the module's physical orientation in the sensor before JPEG
   * encode. Lot 4's camera module is mounted 180 degrees relative to the
   * display viewport, so mirroring horizontally and not vertically makes live
   * preview, saved JPEGs, and QR/camera utility captures agree without any
   * app-level rotation or post-processing.
   */
  bool hmirror = true;
  bool vflip   = false;
  /** White-balance algorithms (auto + AWB-gain). Disable both only for diagnostic captures. */
  bool whitebal = true;
  bool awb_gain = true;
  /** Auto exposure / auto gain. Forcing values manually here makes 90% of "weird color" reports. */
  bool exposure_ctrl = true;
  bool gain_ctrl     = true;
  // OV3660 night-mode AEC can extend frame intervals in low light. Video
  // preview may disable it without disabling automatic exposure/gain. Other
  // sensors retain their existing AEC2 behavior; this is not a frame-rate cap.
  bool allowNightMode = true;
  /** Bad/white pixel correction + gamma + lens shading — keep on for natural color. */
  bool bpc = true;
  bool wpc = true;
  bool raw_gma = true;
  bool lenc = true;
  bool dcw = true;
  /**
   * Load the OV5640 voice-coil-motor autofocus firmware over SCCB after the
   * camera comes up and run the AF. Without this the lens sits at its boot
   * position (~infinity) and anything closer than ~50 cm is soft — fatal for
   * the QR scanner at ~15 cm hold distance.
   *
   * Off by default because the firmware load adds ~150 ms to camera init and
   * polls SCCB; turn it on for apps that need close focus (wifi_setup QR scan,
   * camera_menu_app capture, etc).
   *
   * AF mode is selected by `singleShotAf` (see below).
   */
  bool autofocus = false;

  /**
   * When `autofocus == true`:
   *   - false (default): continuous AF. The VCM tracks the highest-contrast
   *     region of the scene every frame. Right choice for a live viewfinder
   *     with moving subjects (camera_menu_app).
   *   - true: single-shot AF. The firmware sweeps the lens through its full
   *     range once, parks at the highest-contrast position, and stays there.
   *     Right choice for stationary close-up scanning (wifi_setup QR scan):
   *     no hunt-induced blur, deterministic focus once init returns.
   */
  bool singleShotAf = false;
};

// Shared startup/recovery entry point for camera, QR, and diagnostics.
// Powers up, configures, and verifies a first frame. On failure cleans up and
// retries once with one framebuffer at the SAME resolution/quality. Failures
// and memory headroom are queued for Fleet (delivered when networking resumes).
// Callers must quiesce network/audio owners before entering and restore them
// only after deinit succeeds (initialized() becomes false). Use from the UI /
// peripheral owner task; do not concurrently capture or reconfigure the camera.
// frameSize: esp_camera FRAMESIZE_* value (-1 = QXGA default)
// pixelFormat: esp_camera PIXFORMAT_* value (-1 = JPEG default)
// tuning:     optional ISP knobs applied while SCCB is live on accessory I2C.
//             Pass nullptr to use defaults; pass `&yourTuning` to re-apply
//             user choices on every (re)init.
// jpegQualityOverride: esp_camera JPEG quality value. Lower numbers mean
//             higher quality/larger files. Pass -1 to use the HAL default for
//             the selected frame size.
//
// The camera borrows Arduino Wire1's already-installed controller, shared with
// the IMU and codec under the HAL I2C mutex.
bool init(int frameSize = -1, int pixelFormat = -1, const Tuning* tuning = nullptr,
          int jpegQualityOverride = -1);

/** Drop a few frames so AE/AWB converges; called after init() / framesize changes (~1s). */
void settleAutoExposure(uint32_t millisBudget = 700);

/**
 * Tell the OV5640's AF firmware to do one focus sweep, wait for it to settle,
 * and park the VCM at the highest-contrast position. Talks to the sensor over
 * Arduino's Wire1 (SCCB is plain I2C at 0x3C), shared under the HAL mutex.
 *
 * Requires Tuning.autofocus to have been true on the most recent init() (the
 * firmware blob is loaded then; this function only triggers a refocus). Safe
 * to call repeatedly; budget ~2–3 s per call while the VCM physically sweeps
 * its full range, evaluates contrast at each step, and locks on the sharpest
 * position. The OV5640 AF firmware will not clear CMD_ACK until that sweep
 * completes — anything below ~3 s is asking to time out mid-sweep, which
 * leaves the lens parked at an arbitrary intermediate position.
 *
 * Returns false if the camera is not initialized, the sensor doesn't ACK,
 * or the AF firmware doesn't reach "focused" within timeoutMs.
 */
bool triggerAutofocus(uint32_t timeoutMs = 5000);

// Deinitialize camera (releases resources, powers down)
void deinit();

// Get next frame. Returns true and fills *out; caller must call returnFrame() when done.
bool getFrame(CameraFrame* out);

// Return frame buffer to camera driver (must match last getFrame)
void returnFrame();

// Capture image and save to SD card (auto-generated filename)
bool captureToSd();

// Capture JPEG image into a malloc'd buffer. Caller must free(*outBuf) when done.
bool captureJpeg(uint8_t** outBuf, size_t* outLen);

/**
 * Capture one JPEG frame and write it to `path` on SD.
 * Removes the file and returns false if the write fails or the frame cannot be fetched.
 * Camera must already be initialised at the desired framesize before calling.
 */
bool captureToFile(const char* path);

/**
 * Largest contiguous PSRAM block currently available, in kilobytes.
 * Use for pre-flight memory checks before high-res camera init.
 * Returns 0 when PSRAM is not found.
 */
size_t availablePsramKb();

// Check if camera is initialized
bool initialized();
const char* lastInitError();

}  // namespace camera
}  // namespace hal
