#pragma once
#include "core/preview_decoder.h"
#include "ui/native_frame.h"
class TFT_eSprite;
namespace ui {
// UI-task renderer for Camera and recorded-video views. Owns decoder and two PSRAM
// surfaces; no sensor, navigation, capture-workflow or animation-clock ownership.
class CameraPreview {
public:
    using Result = core::images::PreviewDecoder::Result;
    using ShouldAbort = core::images::PreviewDecoder::ShouldAbort;
    using Presentation = NativeFramePresentation;
    CameraPreview() = default;
    ~CameraPreview();
    CameraPreview(const CameraPreview&) = delete;
    CameraPreview& operator=(const CameraPreview&) = delete;
    bool prepareSurfaces();
    // Call after camera DMA setup. QVGA Camera uses native row decoding;
    // cameraVga uses direct crop/rotation. Both retain the legacy allocation
    // fallback. Recorded-video playback normally uses native rows; the reduced
    // decoder trial keeps this ownership and uses the pinned JPEGDEC instead.
    // No surfaces means the app uses its low-memory fallback.
    // reduced=true uses quarter-scale JPEGDEC for HD capture preview, avoiding
    // full-resolution native decode traffic alongside SD writes.
    bool prepareDecoder(bool video=false, bool cameraVga=false, bool reduced=false);
    void closeDecoder();
    void close();
    bool hasSurfaces() const { return frame_ && composite_; }
    bool ready() const { return hasSurfaces() && decoder_.ready(); }
    bool hasFrame() const { return hasSurfaces() && frameReady_; }
    // UI-task copy from an immutable completed native 280x240 decoder result.
    // video=true copies into the playback surface used by presentVideo.
    bool copyDecoded(const uint16_t* pixels, bool video=false);
    // UI-task borrowed 320x240 RGB565 high-byte-first sensor input. Crops the
    // same central 280 columns into native portrait storage, with no allocation.
    // Exact input length required; interrupted/invalid output is not presented.
    Result copyRgb565(const uint8_t* bytes, size_t length, ShouldAbort abort=nullptr, void* user=nullptr);
    // In-place decoding invalidates the retained image until success. On partial
    // failure/interruption, present is a no-op and the panel keeps its last frame.
    // fit preserves the full image with letterboxing, including HD camera input.
    Result decode(const uint8_t* jpeg, size_t length, ShouldAbort abort=nullptr, void* user=nullptr, bool fit=false);
    // Existing overlay phase is stage*1000 + age (0..999); negative pulse age
    // disables the white shutter pulse. Does not advance workflow or clocks.
    Presentation present(unsigned phase, int pulseAgeMs=-1,
                         const char* status=nullptr, bool recording=false, bool tutorial=false);
    // Legacy low-memory QVGA path using shared JPEGDecoder on the UI task.
    // Caller retains view chrome; this clips blocks above the bottom 28px band.
    bool presentFallback(const uint8_t* jpeg, size_t length);
    // Recorded video: fit the entire image with black letterboxing, then show
    // playback controls instead of camera/shutter overlays. Decodes directly to
    // the presentation surface; failure invalidates it. presentVideo restores
    // text bars and the entire progress track without copying the image.
    // UI-task only; call decodeVideo before the first presentVideo.
    Result decodeVideo(const uint8_t* jpeg, size_t length);
    Presentation presentVideo(const char* title, const char* controls, unsigned progress);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    bool copyCompositeForDevelopment(TFT_eSprite& target) const;
#endif
private:
    core::images::PreviewDecoder decoder_;
    uint16_t* frame_=nullptr;
    uint16_t* composite_=nullptr;
    bool frameReady_=false;
};
}
