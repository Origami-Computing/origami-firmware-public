#include "services/camera_trial.h"
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include <Arduino.h>
#include <esp_camera.h>
#include <SD.h>
#include "hal/hal_bus.h"
#include "hal/hal_sd.h"

namespace services { namespace camera {
bool SettleTrial::prepare(const Session& session, uint32_t run, int frames, unsigned repetition) {
  if (!run || frames < -1 || frames > 30 || repetition < 1 || repetition > 9 ||
      !session.running() || !hal::sd::mounted()) {
    Serial.println("[camera-settle] ERR args_or_camera_not_ready");
    return false;
  }
  runId = run; discardFrames = frames; repeat = repetition;
  if (discardFrames < 0) snprintf(setting, sizeof(setting), "T600");
  else snprintf(setting, sizeof(setting), "D%02d", discardFrames);
  snprintf(path, sizeof(path), "/photos/SET_%lu_VGA_%s_R%u.jpg",
           (unsigned long)runId, setting, repeat);
  snprintf(manifestPath, sizeof(manifestPath), "/settle/%lu.csv", (unsigned long)runId);
  if (!hal::sd::ensureDir("/photos") || !hal::sd::ensureDir("/settle") ||
      hal::sd::exists(path)) {
    Serial.println("[camera-settle] ERR storage_or_existing_photo");
    return false;
  }

  return true;
}

void SettleTrial::begin(const hal::camera::Tuning& tuning, const char* mode) {
  Serial.printf("[camera-settle] BEGIN run=%lu setting=%s repeat=%u mode=%s "
                "ae=%d wb=%u gain=%u saturation=%d contrast=%d sharpness=%d\n",
                (unsigned long)runId, setting, repeat, mode,
                tuning.ae_level, tuning.wb_mode, tuning.gainceiling_idx,
                tuning.saturation, tuning.contrast, tuning.sharpness);

  // Match the production VGA restart. Only the post-init settling policy varies.
  // Storage naming/metadata and preview recovery are outside the measured interval.
  startMs = millis();
}

bool SettleTrial::capture(Session& session, const hal::camera::Tuning& tuning,
                          PreparationObserver reportPreparation) {
  session.stop();
  delay(80);
  const auto preparation = session.prepareCapture(false);
  if (reportPreparation) reportPreparation(preparation);
  const bool wifiOk = preparation == Session::PrepareResult::Ready;
  delay(20);
  initStartMs = millis();
  const bool initOk = wifiOk && session.start(FRAMESIZE_VGA, PIXFORMAT_JPEG, &tuning);
  settleStartMs = millis();
  discarded = 0;
  bool framesOk = initOk;
  while (framesOk && (discardFrames < 0 ? millis() - settleStartMs < 600u
                                        : discarded < (uint32_t)discardFrames)) {
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame) {
      framesOk = false;
      break;
    }
    esp_camera_fb_return(frame);
    ++discarded;
    if (discardFrames < 0) delay(20);  // Identical to settleAutoExposure(600).
    if (millis() - settleStartMs > 8000u) framesOk = false;
  }
  acquireStartMs = millis();
  camera_fb_t* frame = framesOk ? esp_camera_fb_get() : nullptr;
  readyMs = millis();
  width = frame ? frame->width : 0;
  height = frame ? frame->height : 0;
  bytes = frame ? frame->len : 0;
  frameStartUs = frame ? (uint64_t)frame->timestamp.tv_sec * 1000000ULL +
                                        frame->timestamp.tv_usec : 0;
  const bool jpegOk = frame && frame->format == PIXFORMAT_JPEG && width == 640 &&
                      height == 480 && bytes >= 4 && frame->buf[0] == 0xff &&
                      frame->buf[1] == 0xd8 && frame->buf[bytes - 2] == 0xff &&
                      frame->buf[bytes - 1] == 0xd9;
  saved = jpegOk && hal::sd::writeFile(path, frame->buf, bytes);
  writeDoneMs = millis();
  if (frame) esp_camera_fb_return(frame);
  if (!saved && hal::sd::exists(path)) hal::sd::removeFile(path);
  return saved;
}

bool SettleTrial::finish(bool restored) {
  const char* header = "run,setting,repeat,discarded,prep_ms,init_ms,settle_ms,acquire_ms,"
                       "ready_ms,frame_dma_start_us,write_ms,width,height,bytes,saved,restored,path";
  char row[320];
  snprintf(row, sizeof(row), "%lu,%s,%u,%lu,%lu,%lu,%lu,%lu,%lu,%llu,%lu,%u,%u,%u,%d,%d,%s",
           (unsigned long)runId, setting, repeat, (unsigned long)discarded,
           (unsigned long)(initStartMs - startMs), (unsigned long)(settleStartMs - initStartMs),
           (unsigned long)(acquireStartMs - settleStartMs), (unsigned long)(readyMs - acquireStartMs),
           (unsigned long)(readyMs - startMs), (unsigned long long)frameStartUs,
           (unsigned long)(writeDoneMs - readyMs), width, height, (unsigned)bytes,
           saved ? 1 : 0, restored ? 1 : 0, path);
  hal::bus::lockSpi();
  const bool newManifest = !SD.exists(manifestPath);
  File manifest = SD.open(manifestPath, FILE_APPEND);
  bool manifestOk = (bool)manifest;
  if (manifest) {
    if (newManifest) manifestOk = manifest.println(header) == strlen(header) + 2;
    manifestOk = (manifest.println(row) == strlen(row) + 2) && manifestOk;
    manifest.close();
  }
  hal::bus::unlockSpi();
  Serial.printf("[camera-settle] ROW %s\n", row);
  Serial.printf("[camera-settle] END ok=%d manifest=%s\n",
                saved && restored && manifestOk ? 1 : 0, manifestPath);
  return saved && restored && manifestOk;
}
} }
#endif
