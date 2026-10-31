#include "services/video_recording.h"
#include "hal/hal_tasks.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <algorithm>
#include <string.h>

namespace services { namespace camera {
bool VideoRecording::beginCamera(Session& camera,uint16_t width,uint16_t height,bool audio,bool preview) {
    if (busy()) return false;
    if (!camera.running() || !camera.backgroundHeld()) {
        close();captureFailed_=false;error_="Camera reservation required";state_=State::Failed;return false;
    }
    if (!beginCommon(width,height,audio)) return false;
    if (preview) previewBuffers_=static_cast<uint8_t*>(heap_caps_malloc(3*kFrameBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    const char* failure=nullptr;
    if (preview && !previewBuffers_) failure="Not enough preview memory";
    else if (!cameraReader_.acquire(camera)) failure="Camera reader busy";
    else {
        cameraAttached_=true;
        previewLatest_=-1;previewBorrowed_=-1;
        for (auto& used:previewUsed_) used=false;
        captureRunning_=true;
        if (!hal::tasks::spawn(hal::tasks::Id::VideoCapture,runCamera,this)) {
            captureRunning_=false;failure="Camera worker unavailable";
        }
    }
    if (!failure) return true;
    close();error_=failure;state_=State::Failed;
    return false;
}
void VideoRecording::closeCamera() {
    while (captureRunning_.load()) vTaskDelay(pdMS_TO_TICKS(1));
    releasePreview();
    previewLatest_=-1;
    cameraReader_.close();
    cameraAttached_=false;
    heap_caps_free(previewBuffers_);previewBuffers_=nullptr;
}
bool VideoRecording::readPreview(hal::camera::CameraFrame* frame) {
    if (!frame || previewBorrowed_>=0 || !cameraAttached_) return false;
    const int slot=previewLatest_.exchange(-1);
    if (slot<0) return false;
    previewBorrowed_=slot;
    *frame=previewFrames_[slot];
    return true;
}
void VideoRecording::releasePreview() {
    if (previewBorrowed_<0) return;
    previewUsed_[previewBorrowed_].store(false);
    previewBorrowed_=-1;
}
void VideoRecording::runCamera(void* self) {
    auto* recording=static_cast<VideoRecording*>(self);
    recording->captureCamera();
    hal::tasks::recordCurrentStack(hal::tasks::Id::VideoCapture);
    // No further instance access after publishing completion.
    recording->captureRunning_.store(false);
    hal::tasks::deleteCurrent(hal::tasks::Id::VideoCapture);
}
void VideoRecording::captureCamera() {
    uint32_t lastFrameMs=millis();
    while (!stopping_.load()) {
        const auto state=state_.load();
        if (state==State::Starting) {vTaskDelay(pdMS_TO_TICKS(1));continue;}
        if (state!=State::Recording) break;
        hal::camera::CameraFrame frame{};
        const uint64_t started=esp_timer_get_time();
        if (cameraReader_.read(&frame)) {
            const uint32_t elapsed=uint32_t(esp_timer_get_time()-started);
            ++captureStats_.reads;captureStats_.readUs+=elapsed;
            captureStats_.maxReadUs=std::max(captureStats_.maxReadUs,elapsed);
            captureStats_.maxBytes=std::max(captureStats_.maxBytes,uint32_t(frame.len));
            const bool valid=frame.buf && frame.len>=4 && frame.len<=kFrameBytes &&
                frame.format==PIXFORMAT_JPEG && frame.width==width_ && frame.height==height_;
            if (valid && (!captureStats_.unique || frame.dmaStartUs!=captureStats_.lastSensorUs)) {
                if (!captureStats_.unique) captureStats_.firstSensorUs=frame.dmaStartUs;
                captureStats_.lastSensorUs=frame.dmaStartUs;++captureStats_.unique;
                lastFrameMs=millis();
                submitFrame(frame.buf,frame.len,uint32_t(frame.dmaStartUs/1000));
                if (previewBuffers_ && !stopping_.load()) for (int slot=0;slot<3;++slot) {
                    bool free=false;
                    if (!previewUsed_[slot].compare_exchange_strong(free,true)) continue;
                    auto* bytes=previewBuffers_+slot*kFrameBytes;
                    memcpy(bytes,frame.buf,frame.len);
                    previewFrames_[slot]=frame;previewFrames_[slot].buf=bytes;
                    // Exchange transfers the previous unpublished-to-UI frame
                    // back to the producer. A UI borrow keeps its own slot busy.
                    const int previous=previewLatest_.exchange(slot);
                    if (previous>=0) previewUsed_[previous].store(false);
                    break;
                }
            }
            cameraReader_.returnFrame();
        }
        if (!stopping_.load() && uint32_t(millis()-lastFrameMs)>2500) {
            captureFailed_=true;stopping_=true;
        }
        // Let the SD writer and idle cleanup run even when sensor reads are ready.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    cameraReader_.returnFrame();
}
} }
