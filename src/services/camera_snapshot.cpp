#include "services/camera_snapshot.h"
#include "services/camera_session.h"
#include "services/camera_capture_policy.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>
namespace services { namespace camera {
Snapshot::~Snapshot() { clear(); }
void Snapshot::clear() { heap_caps_free(data_); data_=nullptr; size_=0; }
Snapshot::Result Snapshot::captureVga(Session& session, uint64_t requestUs) {
    clear();
    timing_ = {};
    hal::camera::CameraFrame frame{};
    bool acquired = false;
    // The driver's own read timeout can exceed this inter-read time budget.
    for (unsigned attempt=0; attempt<8 && esp_timer_get_time()-requestUs<1500000; ++attempt) {
        if (!session.read(&frame)) break;
        if (frame.format != HAL_CAMERA_PIXFORMAT_JPEG ||
            !capture_policy::validVgaJpeg(frame.buf,frame.len,frame.width,frame.height)) {
            session.returnFrame();
            break;
        }
        if (capture_policy::freshFrame(frame.dmaStartUs,requestUs)) { acquired=true; break; }
        ++timing_.stale;
        session.returnFrame();
    }
    if (!acquired) return Result::FrameUnavailable;
    timing_.readyUs=esp_timer_get_time();
    timing_.dmaUs=frame.dmaStartUs;
    data_=static_cast<uint8_t*>(heap_caps_malloc(frame.len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (data_) { size_=frame.len; memcpy(data_,frame.buf,size_); }
    session.returnFrame();
    timing_.copiedUs=esp_timer_get_time();
    return data_ ? Result::Ready : Result::OutOfMemory;
}
} }
