#pragma once
#include "core/preview_decoder.h"
#include "ui/native_frame.h"
class TFT_eSprite;
namespace ui {
// UI-task renderer for the existing Camera view. Owns decoder and two PSRAM
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
    // Call after camera DMA setup. No surfaces means the app uses its fallback.
    bool prepareDecoder();
    void closeDecoder();
    void close();
    bool hasSurfaces() const { return frame_ && composite_; }
    bool ready() const { return hasSurfaces() && decoder_.ready(); }
    bool hasFrame() const { return hasSurfaces() && frameReady_; }
    Result decode(const uint8_t* jpeg, size_t length, ShouldAbort abort=nullptr, void* user=nullptr);
    // Existing overlay phase is stage*1000 + age (0..999); negative pulse age
    // disables the white shutter pulse. Does not advance workflow or clocks.
    // Tutorial footer and photographer are composed before the same native transfer.
    Presentation present(unsigned phase, int pulseAgeMs=-1, bool tutorial=false);
    // Legacy low-memory QVGA path using shared JPEGDecoder on the UI task.
    // Caller retains view chrome; this clips blocks above the bottom 28px band.
    bool presentFallback(const uint8_t* jpeg, size_t length);
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
