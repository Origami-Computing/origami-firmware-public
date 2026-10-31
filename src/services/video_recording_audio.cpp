#include "services/video_recording.h"
#include "services/camera_session.h"
#include "hal/hal_audio.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace services { namespace camera {
bool VideoRecording::beginWithAudio(const Session& camera,uint16_t width,uint16_t height) {
    if (busy()) return false;
    if (!camera.running() || !camera.backgroundHeld()) {
        error_="Camera reservation required";state_=State::Failed;return false;
    }
    return beginCommon(width,height,true);
}
bool VideoRecording::prepareAudio() {
    audioRing_=static_cast<int16_t*>(heap_caps_malloc(kAudioCapacity*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    lastFrame_=static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!audioRing_ || !lastFrame_) {error_="Not enough audio memory";return false;}
    if (!audioPause_.acquire(8000)) {error_="Microphone busy";return false;}
    micSettings_=core::audio::mic_tuning::settings();
    ownsMic_=true; // Unwind partial driver initialization too.
    if (!hal::audio::initMic(true)) {error_="Microphone unavailable";return false;}
    micRunning_=true;
    if (!hal::tasks::spawn(hal::tasks::Id::VideoMic,runMic,this)) {
        micRunning_=false;error_="Microphone worker unavailable";return false;
    }
    return true;
}
void VideoRecording::releaseAudio() {
    stopping_=true;
    while (micRunning_.load()) vTaskDelay(pdMS_TO_TICKS(5));
    if (ownsMic_) {hal::audio::endMicSession();ownsMic_=false;}
    audioPause_.release();
}
void VideoRecording::runMic(void* context) {
    auto* self=static_cast<VideoRecording*>(context);
    self->captureAudio();
    hal::tasks::recordCurrentStack(hal::tasks::Id::VideoMic);
    self->micRunning_.store(false); // Final instance access before UI cleanup.
    hal::tasks::deleteCurrent(hal::tasks::Id::VideoMic);
}
void VideoRecording::captureAudio() {
    int32_t raw[256];int16_t pcm[256];
    core::audio::mic_tuning::Processor processor;
    processor.resetFromSnapshot(micSettings_,core::avi::kAudioRate);
    uint32_t lastData=millis();
    while (!stopping_.load()) {
        size_t bytes=0;
        const auto result=hal::audio::readMic(raw,sizeof(raw),&bytes,pdMS_TO_TICKS(20));
        if (result!=ESP_OK || bytes>sizeof(raw) || bytes%sizeof(int32_t)) {
            micFailure_=MicFailure::Read;stopping_=true;break;
        }
        if (!bytes) {
            if (uint32_t(millis()-lastData)>5000) {
                micFailure_=MicFailure::Read;stopping_=true;break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));continue;
        }
        lastData=millis();
        const uint32_t head=audioHead_.load(),tail=audioTail_.load();
        const uint32_t count=std::min(uint32_t(bytes/sizeof(int32_t)),core::avi::kAudioRate*300-head);
        if (head-tail+count>kAudioCapacity) {micFailure_=MicFailure::Overrun;stopping_=true;break;}
        processor.processRaw(raw,pcm,count);
        for (uint32_t i=0;i<count;++i) audioRing_[(head+i)%kAudioCapacity]=pcm[i];
        if (!head) {
            // Estimate first delivered sample time, before publishing the ring.
            // DMA startup latency still requires physical synchronization testing.
            audioOriginMs_=millis()-uint32_t(uint64_t(bytes/sizeof(int32_t))*1000/core::avi::kAudioRate);
        }
        audioHead_.store(head+count);
        micStarted_.store(true);
        if (head+count==core::avi::kAudioRate*300) {stopping_=true;break;}
    }
}
void VideoRecording::writeAudio() {
    hal::sd::StreamWriter file;
    char temporary[56]{};
    bool ok=hal::sd::ensureDir("/videos"),available=false;
    for (unsigned attempt=0;ok && attempt<16;++attempt) {
        snprintf(path_,sizeof(path_),"/videos/VID_%08lx.avi",(unsigned long)esp_random());
        snprintf(temporary,sizeof(temporary),"%s.part",path_);
        if (!hal::sd::exists(path_) && !hal::sd::exists(temporary)) {available=true;break;}
    }
    ok=ok && available && hal::sd::openWriter(temporary,&file);
    uint8_t header[core::avi::kAudioHeaderBytes];
    core::avi::audioHeader(header,width_,height_,0,0,0,0,0,0);
    if (ok) ok=hal::sd::writeStream(&file,header,sizeof(header));
    uint32_t movieBytes=0,entries=0,count=0,largest=0,lastLength=0;
    auto chunk=[&](const uint8_t* data,uint32_t bytes,bool audio) {
        if (entries>=kMaxFrames*2 || movieBytes+core::avi::chunkBytes(bytes)>256u*1024*1024-400000u) return false;
        uint8_t tag[8],pad=0;core::avi::chunk(tag,bytes,audio);
        const uint32_t writeStarted=millis();
        if (!hal::sd::writeStream(&file,tag,8) || (bytes && !hal::sd::writeStream(&file,data,bytes)) ||
            ((bytes&1) && !hal::sd::writeStream(&file,&pad,1))) return false;
        const uint32_t elapsed=uint32_t(millis()-writeStarted);
        writerStats_.chunkWriteMs+=elapsed;
        writerStats_.maxChunkWriteMs=std::max(writerStats_.maxChunkWriteMs,elapsed);
        if (audio) writerStats_.pcmBytes+=bytes;
        else if (bytes) {++writerStats_.jpegChunks;writerStats_.jpegBytes+=bytes;}
        core::avi::index(index_+entries*16,movieBytes+4,bytes,audio);
        ++entries;movieBytes+=core::avi::chunkBytes(bytes);return true;
    };
    auto audio=[&]() {
        // Drain a bounded snapshot before another JPEG. One PCM chunk per
        // image is insufficient when SD writes fall below 24 images/second:
        // microphone input continues at 96 KB/s regardless of video cadence.
        const uint32_t head=audioHead_.load();
        uint32_t tail=audioTail_.load();
        const bool finalDrain=stopping_.load() && !micRunning_.load();
        while (head!=tail) {
            if (head-tail<core::avi::kAudioChunkBytes/2 && !finalDrain) break;
            const uint32_t samples=std::min(std::min(head-tail,kAudioCapacity-tail%kAudioCapacity),core::avi::kAudioChunkBytes/2);
            if (!chunk(reinterpret_cast<const uint8_t*>(audioRing_+tail%kAudioCapacity),samples*2,true)) return false;
            tail+=samples;
            audioTail_.store(tail);audioWritten_.store(tail);
        }
        return true;
    };
    auto video=[&](const uint8_t* jpeg,uint32_t length,bool repeated,bool finalFrame=false) {
        // AVI dropped-frame chunks preserve the clock without writing another JPEG.
        // Bootstrap with an image: a hold is valid only after a real frame.
        // The interval before each new image is explicit too, so stopping
        // immediately after it leaves an unambiguous one-tick EOF duration.
        const uint32_t storedLength=repeated && count && !finalFrame ? 0 : length;
        if (count>=kTimelineFps*300 || !chunk(jpeg,storedLength,false)) return false;
        if (repeated && storedLength) {
            ++writerStats_.repeatedJpegChunks;writerStats_.repeatedJpegBytes+=storedLength;
        }
        largest=std::max(largest,length);frames_.store(++count);
        if (repeated) repeatedFrames_.fetch_add(1);
        return true;
    };
    while (ok && !micStarted_.load() && micRunning_.load() && !stopping_.load()) vTaskDelay(pdMS_TO_TICKS(1));
    if (ok && micStarted_.load()) state_=State::Recording;
    while (ok && micFailure_.load()==MicFailure::None) {
        // Prioritize PCM so a camera/display stall cannot silently lose sound.
        if (!audio()) {ok=false;break;}
        const uint32_t tail=tail_.load();
        if (tail!=head_.load()) {
            const Slot slot=slots_[tail%kSlots];
            const uint8_t* jpeg=buffers_+(tail%kSlots)*kFrameBytes;
            const int32_t delta=int32_t(slot.at-audioOriginMs_.load());
            if (delta>=0 && uint32_t(delta)<kMaxDurationMs) {
                const uint32_t target=uint64_t(uint32_t(delta))*kTimelineFps/1000;
                if (!lastLength) {memcpy(lastFrame_,jpeg,slot.length);lastLength=slot.length;}
                while (ok && count<target) {
                    // Empty chunks retain the prior picture without rewriting its JPEG.
                    // Playback expands timestamps explicitly; final holds below
                    // still provide consecutive end-of-stream samples.
                    ok=audio() && video(lastFrame_,lastLength,true);
                    if (movieBytes>=240u*1024*1024) {stopping_=true;break;}
                }
                if (ok && count==target) {
                    ok=video(jpeg,slot.length,false);
                    if (ok) {memcpy(lastFrame_,jpeg,slot.length);lastLength=slot.length;}
                }
                // Multiple sensor timestamps may map to the same 30 Hz slot.
                // Only a published image can become the source for later holds;
                // replacing it with a discarded submission changes the image
                // inside an interval that was supposed to repeat the previous one.
            }
            tail_.store(tail+1);
        } else if (stopping_.load() && !captureRunning_.load() && !micRunning_.load() && audioHead_.load()==audioTail_.load()) break;
        else vTaskDelay(pdMS_TO_TICKS(1));
        if (movieBytes>=240u*1024*1024 || count>=kTimelineFps*300) stopping_=true;
    }
    stopping_=true;state_=State::Finishing;
    while (micRunning_.load()) vTaskDelay(pdMS_TO_TICKS(1));
    if (micFailure_.load()!=MicFailure::None) ok=false;
    while (ok && audioTail_.load()!=audioHead_.load()) ok=audio();
    // Keep the final image through the final PCM sample; repeated frames are
    // reported separately and never counted as sensor/capture throughput.
    // Keep final padding explicit so readers see timestamped EOF samples.
    // If a new image itself ends a sparse gap, converters must still bound
    // inferred EOF duration by the AVI's declared timeline frame count.
    const uint32_t required=(audioWritten_.load()+core::avi::kAudioRate/kTimelineFps-1)/(core::avi::kAudioRate/kTimelineFps);
    while (ok && lastLength && count<required) ok=video(lastFrame_,lastLength,true,count+2>=required);
    if (ok && count && audioWritten_.load()) {
        uint8_t tag[8];memcpy(tag,"idx1",4);core::avi::u32(tag+4,entries*16);
        ok=hal::sd::writeStream(&file,tag,8) && hal::sd::writeStream(&file,index_,entries*16);
        core::avi::audioHeader(header,width_,height_,count,uint64_t(count)*1000/kTimelineFps,movieBytes,largest,audioWritten_.load(),entries);
        core::avi::u32(header+128,1);core::avi::u32(header+132,kTimelineFps);
        ok=ok && hal::sd::seekWriter(&file,0) && hal::sd::writeStream(&file,header,sizeof(header));
    } else ok=false;
    const bool closed=hal::sd::closeWriter(&file);
    ok=ok && closed;
    if (ok) ok=hal::sd::replaceFile(temporary,path_);
    if (!ok) {
        if (!entries && temporary[0]) hal::sd::removeFile(temporary);
        error_=micFailure_.load()==MicFailure::Overrun?"Audio buffer overrun":
            micFailure_.load()==MicFailure::Read?"Microphone read failed":!count?"No video frames captured":"Video SD write failed";
    }
    state_.store(ok?State::Saved:State::Failed); // Both workers have stopped using shared storage.
}
} }
