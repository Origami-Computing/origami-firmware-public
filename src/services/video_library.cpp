#include "services/video_library.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include "hal/hal_wifi.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <algorithm>

namespace services { namespace video {
bool Library::open(bool deferred) {
    if (!opening_) close();
    error_="";
    if (!hal::sd::mounted()) { close();error_="Insert SD card"; return false; }
    const auto admission=deferred ? reservation_.pollAcquire(true) : reservation_.acquire(true,8000);
    opening_=admission==background_network::PeripheralReservation::Result::Pending;
    if (opening_) { error_="Preparing preview...";return false; }
    if (admission!=background_network::PeripheralReservation::Result::Acquired) {
        error_="Network busy; try again"; return false;
    }
    // Match photo browsing: radio transitions compete with SD/display work and
    // scarce internal decoder memory. Transport is already acknowledged paused.
    radioStopped_=true;
    hal::wifi::stop();
    paths_=static_cast<char*>(heap_caps_malloc(kFiles*kPath,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!paths_) { error_="Not enough memory"; close(); return false; }
    count_=hal::sd::listFiles("/videos",".avi",paths_,kFiles,kPath);
    return true;
}
const char* Library::path(int index) const {
    return paths_ && index>=0 && index<count_ ? paths_+index*kPath : "";
}
void Library::stop() {
    cancel_.store(true);
    audio_.pause();
    while (running_.load()) vTaskDelay(pdMS_TO_TICKS(5));
    // WavPlayer may defer cleanup on a stop timeout. This session retains its
    // stronger transport/radio admission until the same owner acknowledges it.
    do {
        audio_.closeSession();
        if (audio_.sessionOpen()) vTaskDelay(pdMS_TO_TICKS(5));
    } while (audio_.sessionOpen());
    audioStarted_=false;
    borrowed_=false;
    heap_caps_free(buffers_); buffers_=nullptr;
    heap_caps_free(index_); index_=nullptr;
    requested_=0;
    head_=0;tail_=0;state_=State::Idle;
}
void Library::close() {
    opening_=false;
    stop(); heap_caps_free(paths_); paths_=nullptr; count_=0;
    if (radioStopped_) {radioStopped_=false;hal::wifi::scheduleRestore();}
    reservation_.release();
}
bool Library::select(int index) {
    stop(); error_=""; info_={};
    if (!reservation_.held() || index<0 || index>=count_) { error_="Video unavailable"; state_=State::Failed; return false; }
    selected_=index;
    buffers_=static_cast<uint8_t*>(heap_caps_malloc(kSlots*kFrameBytes+kReadScratchBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!buffers_) { error_="Not enough memory"; state_=State::Failed; return false; }
    cancel_=false; running_=true; state_=State::Loading;
    if (!hal::tasks::spawn(hal::tasks::Id::VideoRead,run,this)) {
        running_=false; heap_caps_free(buffers_);buffers_=nullptr;
        error_="Video worker unavailable";state_=State::Failed;return false;
    }
    return true;
}
bool Library::take(Frame& frame, uint32_t atOrBefore) {
    if (borrowed_) return false;
    if (atOrBefore!=UINT32_MAX && atOrBefore>requested_.load()) requested_.store(atOrBefore);
    uint32_t tail=tail_.load();
    const uint32_t head=head_.load();
    if (tail==head) return false;
    if (atOrBefore!=UINT32_MAX) {
        if (numbers_[tail%kSlots]>atOrBefore) return false;
        while (tail+1<head && numbers_[(tail+1)%kSlots]<=atOrBefore) ++tail;
        tail_.store(tail);
    }
    borrowed_=true;
    frame.data=buffers_+(tail%kSlots)*kFrameBytes;
    frame.length=lengths_[tail%kSlots];
    frame.number=numbers_[tail%kSlots];
    frame.imageNumber=imageNumbers_[tail%kSlots];
    return true;
}
bool Library::startAudio() {
    if (state()!=State::Ready && state()!=State::End) {error_="Video not ready";return false;}
    if (!info_.audioSamples) return true;
    if (!audioStarted_) {
        if (!audio_.openSession() || !audio_.startTrack(path(selected_))) {
            error_="Audio playback unavailable";return false;
        }
        audioStarted_=true;
    } else if (audio_.failed()) {
        error_="Unreadable video audio";return false;
    } else audio_.resume();
    return true;
}
void Library::release() {
    if (borrowed_) {borrowed_=false;tail_.store(tail_.load()+1);}
}
void Library::run(void* context) {
    auto* self=static_cast<Library*>(context);
    self->read();
    hal::tasks::recordCurrentStack(hal::tasks::Id::VideoRead);
    self->running_.store(false); // Last access to instance; UI may now reclaim it.
    hal::tasks::deleteCurrent(hal::tasks::Id::VideoRead);
}
void Library::read() {
    hal::sd::StreamReader file;
    uint32_t readMs=0,seekMs=0,queueMs=0,readCalls=0,readBytes=0;
    const uint32_t started=millis();
    auto exact=[&](uint8_t* data,size_t bytes) {
        while (bytes && !cancel_.load()) {
            // Retain bounded byte chunks and cancellation checkpoints while
            // reducing shared-bus handoffs. No extra read buffer is needed.
            const size_t chunk=bytes>8192?8192:bytes;
            const uint32_t before=millis();
            const size_t got=hal::sd::readStream(&file,data,chunk);
            readMs+=uint32_t(millis()-before);++readCalls;readBytes+=got;
            if (!got) return false;
            data+=got;bytes-=got;
        }
        return bytes==0;
    };
    auto seek=[&](uint32_t position) {
        if (cancel_.load()) return false;
        if (file.position==position) return true;
        const uint32_t before=millis();
        const bool ok=hal::sd::seekStream(&file,position);
        seekMs+=uint32_t(millis()-before);return ok;
    };
    uint8_t header[core::avi::kAudioHeaderBytes];
    bool ok=hal::sd::openStream(path(selected_),&file) && file.size<=UINT32_MAX &&
        exact(header,core::avi::kHeaderBytes);
    size_t headerBytes=core::avi::kHeaderBytes;
    if (ok && core::avi::read32(header+56)==2) {
        ok=exact(header+headerBytes,core::avi::kAudioHeaderBytes-headerBytes);
        headerBytes=core::avi::kAudioHeaderBytes;
    }
    ok=ok && core::avi::parse(header,uint32_t(file.size),info_,headerBytes);
    uint32_t videos=0;
    if (ok) {
        index_=static_cast<core::avi::IndexedFrame*>(heap_caps_malloc(
            info_.frames*sizeof(core::avi::IndexedFrame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if (!index_) {error_="Not enough video index memory";ok=false;}
    }
    uint8_t tag[8];
    ok=ok && seek(info_.movieEnd) && exact(tag,8) && !memcmp(tag,"idx1",4) &&
        core::avi::read32(tag+4)==info_.indexEntries*16;
    if (ok) {
        core::avi::IndexBuilder builder(info_,index_,info_.frames);
        uint8_t* scratch=buffers_+kSlots*kFrameBytes;
        for (uint32_t at=0;ok && at<info_.indexEntries;) {
            const uint32_t entries=std::min(uint32_t(kReadScratchBytes/16),info_.indexEntries-at);
            ok=exact(scratch,entries*16);
            for (uint32_t i=0;ok && i<entries;++i) ok=builder.append(scratch+i*16);
            at+=entries;
        }
        ok=ok && builder.complete();
    }
    if (ok && !cancel_.load()) state_.store(State::Ready);
    uint32_t next=0;
    while (ok && next<info_.frames && !cancel_.load()) {
        const uint32_t waitStarted=millis();
        while (head_.load()-tail_.load()>=kSlots && !cancel_.load()) vTaskDelay(pdMS_TO_TICKS(5));
        queueMs+=uint32_t(millis()-waitStarted);
        if (cancel_.load()) break;
        next=std::max(next,std::min(requested_.load(),info_.frames-1));
        const auto entry=index_[next];
        const uint32_t head=head_.load(),slot=head%kSlots,previous=(head+kSlots-1)%kSlots;
        uint8_t* dst=buffers_+slot*kFrameBytes;
        // The preceding published slot cannot be overwritten by this producer
        // while this iteration runs. Consumers only borrow, never mutate it.
        if (head && sources_[previous]==entry.imageNumber) {
            memcpy(dst,buffers_+previous*kFrameBytes,entry.bytes);
            imageNumbers_[slot]=imageNumbers_[previous];
        } else {
            core::avi::Chunk chunk;
            ok=seek(entry.offset) && exact(tag,8) && core::avi::parseChunk(tag,entry.offset,info_,chunk) &&
                !chunk.audio && chunk.bytes==entry.bytes && exact(dst,entry.bytes) && dst[0]==0xff && dst[1]==0xd8;
            if (!ok) break;
            const bool same=head && lengths_[previous]==entry.bytes &&
                !memcmp(dst,buffers_+previous*kFrameBytes,entry.bytes);
            imageNumbers_[slot]=same?imageNumbers_[previous]:entry.imageNumber;
        }
        lengths_[slot]=entry.bytes;sources_[slot]=entry.imageNumber;numbers_[slot]=next++;
        head_.store(head+1);++videos;
    }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    Serial.printf("[video-sd] read_wait_us=%lu read_io_us=%lu seek_wait_us=%lu seek_io_us=%lu\n",
        (unsigned long)file.readWaitUs,(unsigned long)file.readIoUs,
        (unsigned long)file.seekWaitUs,(unsigned long)file.seekIoUs);
    Serial.printf("[video-sd-handoff] read_us=%lu seek_us=%lu\n",
        (unsigned long)file.readHandoffUs,(unsigned long)file.seekHandoffUs);
    Serial.printf("[video-sd-max] read_io_us=%lu seek_io_us=%lu\n",
        (unsigned long)file.readMaxIoUs,(unsigned long)file.seekMaxIoUs);
    Serial.printf("[video-read] elapsed_ms=%lu read_ms=%lu seek_ms=%lu queue_ms=%lu calls=%lu bytes=%lu frames=%lu cancelled=%d valid=%d\n",
        (unsigned long)(millis()-started),(unsigned long)readMs,(unsigned long)seekMs,
        (unsigned long)queueMs,(unsigned long)readCalls,(unsigned long)readBytes,
        (unsigned long)videos,cancel_.load(),ok);
#endif
    hal::sd::closeStream(&file);
    if (!cancel_.load()) {
        if (!ok && !*error_) error_="Invalid or unreadable video";
        state_.store(ok?State::End:State::Failed);
    }
}
} }
