#pragma once
#include <stdint.h>
#include "services/mic_capture.h"
namespace services { namespace agent_voice {
enum class StartResult { Started, MemoryUnavailable, MicUnavailable };
// Prepare reserves background capacity. The app may then release competing
// view sprites before start() allocates the PCM ring and starts the microphone.
bool prepare();
// Call only after prepare() succeeds. Repeated start during capture is a no-op.
StartResult start(services::audio::MicCapture::SampleSink sink, void* user=nullptr);
void tick();
bool isCapturing();
uint32_t finish(); // Idempotent; retains the ring until transport releases it.
void release(); // UI task: ring and reservation after capture finishes.
void requestRelease(); // Worker-safe: defer cleanup to the UI task.
void poll(); // UI task: perform pending cleanup once capture has stopped.
void resetForEntry(); // No live capture is permitted on app entry.
void restorePlayback();
const core::audio::RecordingWaveform& waveform();
uint32_t samples();
int16_t* pcm();
uint32_t capacity();
void releaseBuffers(); // UI task after capture stops; detaches transport before freeing.
void releaseResources();
} }
