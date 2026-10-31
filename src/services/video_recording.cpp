#include "services/video_recording.h"
#include "core/jpeg_envelope.h"
#include "core/mjpeg_avi.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

namespace services { namespace camera {
VideoRecording::~VideoRecording() { close(); }
bool VideoRecording::busy() const {
    const auto s = state();
    return captureRunning_.load() || s == State::Starting || s == State::Recording || s == State::Finishing;
}
void VideoRecording::releaseBuffers() {
    heap_caps_free(buffers_); heap_caps_free(index_);
    buffers_ = index_ = nullptr;
    heap_caps_free(audioRing_);audioRing_=nullptr;
    heap_caps_free(lastFrame_);lastFrame_=nullptr;
}
bool VideoRecording::begin(uint16_t width, uint16_t height) {
    return beginCommon(width,height,false);
}
bool VideoRecording::beginCommon(uint16_t width, uint16_t height,bool withAudio) {
    if (busy()) return false;
    close();
    withAudio_=withAudio;
    captureFailed_=false;captureStats_={};
    error_ = ""; path_[0] = 0; submissions_ = {}; writerStats_ = {};
    head_ = 0; tail_ = 0; frames_ = 0; stopping_ = false;
    audioHead_=0;audioTail_=0;audioWritten_=0;repeatedFrames_=0;
    micStarted_=false;micFailure_=MicFailure::None;
    if (!hal::sd::mounted()) {
        error_ = "Insert SD card"; state_ = State::Failed; return false;
    }
    if (!core::avi::supportedDimensions(width,height)) {
        error_ = "Unsupported video size"; state_ = State::Failed; return false;
    }
    width_ = width; height_ = height;
    buffers_ = static_cast<uint8_t*>(heap_caps_malloc(kSlots * kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    index_ = static_cast<uint8_t*>(heap_caps_malloc(kMaxFrames * 16 * (withAudio_?2:1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffers_ || !index_) {
        releaseBuffers(); error_ = "Not enough video memory"; state_ = State::Failed; return false;
    }
    if (withAudio_ && !prepareAudio()) {
        releaseAudio();releaseBuffers();state_=State::Failed;return false;
    }
    state_ = State::Starting;
    if (!hal::tasks::spawn(hal::tasks::Id::VideoWrite, run, this)) {
        releaseAudio();releaseBuffers(); error_ = "Video worker unavailable"; state_ = State::Failed; return false;
    }
    return true;
}
bool VideoRecording::submit(const uint8_t* jpeg, size_t length, uint32_t now) {
    if (cameraAttached_) return false;
    return submitFrame(jpeg,length,now);
}
bool VideoRecording::submitFrame(const uint8_t* jpeg, size_t length, uint32_t now) {
    if (state() != State::Recording || stopping_.load()) return false;
    const uint32_t head = head_.load(), tail = tail_.load();
    if (!jpeg || length < 4) { ++submissions_.invalid; return false; }
    if (length > kFrameBytes) { ++submissions_.oversized; return false; }
    if (!core::images::singleJpegEnvelope(jpeg, length)) { ++submissions_.invalid; return false; }
    if (head - tail >= kSlots) { ++submissions_.queueFull; return false; }
    const uint32_t slot = head % kSlots;
    memcpy(buffers_ + slot * kFrameBytes, jpeg, length);
    slots_[slot] = {static_cast<uint32_t>(length), now};
    ++submissions_.accepted;
    if (head-tail+1 > submissions_.maxQueued) submissions_.maxQueued=head-tail+1;
    head_.store(head + 1); // Publishes bytes/metadata to the single consumer.
    return true;
}
void VideoRecording::stop() {
    if (busy()) stopping_.store(true);
}
void VideoRecording::close() {
    stop();
    closeCamera();
    while (busy()) vTaskDelay(pdMS_TO_TICKS(5));
    releaseAudio();
    releaseBuffers();
}
void VideoRecording::run(void* self) {
    static_cast<VideoRecording*>(self)->write();
    hal::tasks::recordCurrentStack(hal::tasks::Id::VideoWrite);
    hal::tasks::deleteCurrent(hal::tasks::Id::VideoWrite);
}
void VideoRecording::write() {
    if (withAudio_) {writeAudio();return;}
    hal::sd::StreamWriter file;
    char temporary[56]{};
    bool ok = hal::sd::ensureDir("/videos");
    bool available = false;
    for (unsigned attempt = 0; ok && attempt < 16; ++attempt) {
        snprintf(path_, sizeof(path_), "/videos/VID_%08lx.avi", (unsigned long)esp_random());
        snprintf(temporary, sizeof(temporary), "%s.part", path_);
        if (!hal::sd::exists(path_) && !hal::sd::exists(temporary)) { available = true; break; }
    }
    ok = ok && available && hal::sd::openWriter(temporary, &file);
    uint8_t header[core::avi::kHeaderBytes];
    core::avi::header(header, width_, height_, 0, 0, 0, 0);
    if (ok) ok = hal::sd::writeStream(&file, header, sizeof(header));
    uint32_t count = 0, movieBytes = 0, largest = 0, firstAt = 0, lastAt = 0;
    if (ok) state_.store(State::Recording);
    while (ok) {
        const uint32_t tail = tail_.load();
        if (tail == head_.load()) {
            if (stopping_.load() && !captureRunning_.load()) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const auto slot = slots_[tail % kSlots];
        const uint8_t* jpeg = buffers_ + (tail % kSlots) * kFrameBytes;
        uint8_t chunk[8]; core::avi::chunk(chunk, slot.length);
        const uint8_t pad = 0;
        ok = hal::sd::writeStream(&file, chunk, sizeof(chunk)) &&
             hal::sd::writeStream(&file, jpeg, slot.length) &&
             (!(slot.length & 1) || hal::sd::writeStream(&file, &pad, 1));
        if (!ok) break;
        core::avi::index(index_ + count * 16, movieBytes + 4, slot.length);
        movieBytes += core::avi::chunkBytes(slot.length);
        if (slot.length > largest) largest = slot.length;
        if (!count) firstAt = slot.at;
        lastAt = slot.at;
        frames_.store(++count);
        tail_.store(tail + 1); // Slot can now be reused by producer.
        if (count == kMaxFrames || lastAt - firstAt >= kMaxDurationMs || movieBytes >= 256u * 1024 * 1024) break;
    }
    state_.store(State::Finishing);
    if (ok && count) {
        uint8_t chunk[8]; memcpy(chunk, "idx1", 4); core::avi::u32(chunk + 4, count * 16);
        ok = hal::sd::writeStream(&file, chunk, 8) && hal::sd::writeStream(&file, index_, count * 16);
        const uint32_t span = lastAt - firstAt;
        const uint32_t lastFrameMs = count > 1 ? span / (count - 1) : kSingleFrameDurationMs;
        // A long sensor gap must not extend a bounded recording by another
        // mean frame interval (two frames five minutes apart meant ten minutes).
        const uint64_t estimatedDuration = uint64_t(span) + (lastFrameMs ? lastFrameMs : 1);
        const uint32_t duration = estimatedDuration > kMaxDurationMs ? kMaxDurationMs : uint32_t(estimatedDuration);
        core::avi::header(header, width_, height_, count, duration, movieBytes, largest);
        ok = ok && hal::sd::seekWriter(&file, 0) && hal::sd::writeStream(&file, header, sizeof(header));
    }
    const bool closed=hal::sd::closeWriter(&file);
    ok=ok && closed;
    if (ok && count) ok = hal::sd::replaceFile(temporary, path_);
    if (!ok || !count) {
        // Leave non-empty failed .part files for recovery; never advertise them as playable.
        if (!count && temporary[0]) hal::sd::removeFile(temporary);
        error_ = ok ? "No video frames captured" : "Video SD write failed";
    }
    // Final publication: no further access to this instance from the worker.
    state_.store(ok && count ? State::Saved : State::Failed);
}
} }
