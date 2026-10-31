#include "services/camera_preview_decode.h"
#include "core/jpeg_envelope.h"
#include "core/mjpeg_avi.h"
#include "hal/hal_tasks.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>
#include <assert.h>

namespace services { namespace camera {
bool PreviewDecode::begin(bool fit) {
    close();
    fit_=fit;failed_=false;
    for (auto& slot : slots_) {
        slot.jpeg=static_cast<uint8_t*>(heap_caps_malloc(core::avi::kJpegFrameBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        slot.pixels=static_cast<uint16_t*>(heap_caps_aligned_calloc(16,1,core::images::PreviewDecoder::kPixels*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if (!slot.jpeg || !slot.pixels) {close();return false;}
    }
    if (!decoder_.prepareNative(!fit)) {close();return false;}
    stop_=false;running_=true;
    if (!hal::tasks::spawn(hal::tasks::Id::VideoPreview,task,this)) {
        running_=false;close();return false;
    }
    return true;
}
bool PreviewDecode::submit(const uint8_t* bytes,size_t size,uint64_t sensorUs) {
    if (!active() || stop_ || !bytes || size>core::avi::kJpegFrameBytes ||
        !core::images::singleJpegEnvelope(bytes,size)) return false;
    for (auto& slot : slots_) {
        State expected=Free;
        if (!slot.state.compare_exchange_strong(expected,Writing)) continue;
        memcpy(slot.jpeg,bytes,size);slot.bytes=size;slot.sensorUs=sensorUs;
        slot.state=Queued;
        return true;
    }
    return false; // Keep UI bounded; no wait for a decoder/output borrower.
}
bool PreviewDecode::acquire(Frame& frame) {
    if (borrowed_>=0) return false;
    int oldest=-1;
    for (int i=0;i<2;++i) if (slots_[i].state==Ready &&
        (oldest<0 || slots_[i].sensorUs<slots_[oldest].sensorUs)) oldest=i;
    if (oldest>=0) {
        const int i=oldest;
        State expected=Ready;
        if (!slots_[i].state.compare_exchange_strong(expected,Borrowed)) return false;
        borrowed_=i;
        frame={slots_[i].pixels,slots_[i].sensorUs,slots_[i].decodeUs};
        return true;
    }
    return false;
}
void PreviewDecode::release() {
    if (borrowed_<0) return;
    slots_[borrowed_].state=Free;borrowed_=-1;
}
void PreviewDecode::close() {
    assert(borrowed_<0);
    stop_=true;
    while (running_) vTaskDelay(pdMS_TO_TICKS(1));
    decoder_.close();
    for (auto& slot : slots_) {
        heap_caps_free(slot.jpeg);heap_caps_free(slot.pixels);
        slot.jpeg=nullptr;slot.pixels=nullptr;slot.state=Free;
    }
}
void PreviewDecode::task(void* context) {
    auto& self=*static_cast<PreviewDecode*>(context);
    self.run();
    hal::tasks::recordCurrentStack(hal::tasks::Id::VideoPreview);
    self.running_=false; // No subsequent access to self or its codec/buffers.
    hal::tasks::deleteCurrent(hal::tasks::Id::VideoPreview);
}
bool PreviewDecode::shouldAbort(void* context) {
    auto& self=*static_cast<PreviewDecode*>(context);
    return self.stop_;
}
void PreviewDecode::run() {
    unsigned next=0;
    while (!stop_) {
        auto& slot=slots_[next];next^=1;
        State expected=Queued;
        if (slot.state.compare_exchange_strong(expected,Decoding)) {
            const uint64_t started=esp_timer_get_time();
            const auto result=decoder_.decode(slot.jpeg,slot.bytes,slot.pixels,shouldAbort,this,fit_);
            slot.decodeUs=uint32_t(esp_timer_get_time()-started);
            if (result==core::images::PreviewDecoder::Result::Invalid && !stop_) failed_=true;
            slot.state=result==core::images::PreviewDecoder::Result::Decoded && !stop_ ? Ready : Free;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
} }
