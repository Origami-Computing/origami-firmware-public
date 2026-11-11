static void playToneSamples() {
  if (!hal::audio::txReady()) return;
  int16_t buf[256];
  core::dsp::generateSine(buf, 256, s_tonePhase, 440.0f, 44100.0f);
  size_t w = 0;
  hal::audio::txWrite(buf, 256, &w, portMAX_DELAY);
}
