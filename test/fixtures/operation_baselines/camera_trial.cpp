bool runSettleTrial(uint32_t runId, int discardFrames, unsigned repeat) {
  if (!runId || discardFrames < -1 || discardFrames > 30 || repeat < 1 || repeat > 9 ||
      NavHandler::instance().top() != &app || camState != CAM_VIEWFINDER ||
      !s_camera.running() || !hal::sd::mounted()) {
    Serial.println("[camera-settle] ERR args_or_camera_not_ready");
    return false;
  }
  char setting[12];
  if (discardFrames < 0) snprintf(setting, sizeof(setting), "T600");
  else snprintf(setting, sizeof(setting), "D%02d", discardFrames);
  char path[64], manifestPath[64];
  snprintf(path, sizeof(path), "/photos/SET_%lu_VGA_%s_R%u.jpg",
           (unsigned long)runId, setting, repeat);
  snprintf(manifestPath, sizeof(manifestPath), "/settle/%lu.csv", (unsigned long)runId);
  if (!hal::sd::ensureDir("/photos") || !hal::sd::ensureDir("/settle") ||
      hal::sd::exists(path)) {
    Serial.println("[camera-settle] ERR storage_or_existing_photo");
    return false;
  }

  const hal::camera::Tuning tuning = effectiveTuning();
  Serial.printf("[camera-settle] BEGIN run=%lu setting=%s repeat=%u mode=%s "
                "ae=%d wb=%u gain=%u saturation=%d contrast=%d sharpness=%d\n",
                (unsigned long)runId, setting, repeat, kModeLabels[s_cameraMode],
                tuning.ae_level, tuning.wb_mode, tuning.gainceiling_idx,
                tuning.saturation, tuning.contrast, tuning.sharpness);

  // Match the production VGA restart. Only the post-init settling policy varies.
  // Storage naming/metadata and preview recovery are outside the measured interval.
  const uint32_t startMs = millis();
  camShowShutterFrame();
  s_camera.stop();
  delay(80);
  const bool wifiOk = camStopWifiForCamera(false);
  delay(20);
  const uint32_t initStartMs = millis();
  const bool initOk = wifiOk && s_camera.start(FRAMESIZE_VGA, PIXFORMAT_JPEG, &tuning);
  const uint32_t settleStartMs = millis();
  uint32_t discarded = 0;
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
  const uint32_t acquireStartMs = millis();
  camera_fb_t* frame = framesOk ? esp_camera_fb_get() : nullptr;
  const uint32_t readyMs = millis();
  const uint16_t width = frame ? frame->width : 0;
  const uint16_t height = frame ? frame->height : 0;
  const size_t bytes = frame ? frame->len : 0;
  const uint64_t frameStartUs = frame ? (uint64_t)frame->timestamp.tv_sec * 1000000ULL +
                                        frame->timestamp.tv_usec : 0;
  const bool jpegOk = frame && frame->format == PIXFORMAT_JPEG && width == 640 &&
                      height == 480 && bytes >= 4 && frame->buf[0] == 0xff &&
                      frame->buf[1] == 0xd8 && frame->buf[bytes - 2] == 0xff &&
                      frame->buf[bytes - 1] == 0xd9;
  const bool saved = jpegOk && hal::sd::writeFile(path, frame->buf, bytes);
  const uint32_t writeDoneMs = millis();
  if (frame) esp_camera_fb_return(frame);
  if (!saved && hal::sd::exists(path)) hal::sd::removeFile(path);
  if (saved) {
    char label[24];
    snprintf(label, sizeof(label), "%s R%u", setting, repeat);
    (void)camShowCapturedPreview(path, label);
    delay(900);
  }
  const bool restored = camRestorePreviewAfterCapture(500);

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
#endif