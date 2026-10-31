#include <esp_arduino_version.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdint.h>
#include <string.h>

// Arduino 2.0.17 ships esp32-camera as a precompiled SDK archive. Keep this
// compatibility fix local instead of patching the machine's PlatformIO SDK.
// Revalidate both linker wraps and cam_obj_t's prefix when upgrading the core.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate camera DMA compatibility against the upgraded core");

namespace {
// First five uint32_t members of the bundled driver's cam_obj_t (ll_cam.h).
// Copy only this prefix: no aliasing or dependency on the rest of the private ABI.
struct DmaGeometry {
  uint32_t bytesPerItem;
  uint32_t bufferSize;
  uint32_t chunkSize;
  uint32_t chunkCount;
  uint32_t nodeSize;
};
static_assert(sizeof(DmaGeometry) == 20, "Camera DMA prefix must stay packed");
bool s_internalJpeg = false;
#if defined(ORIGAMI_RGB_PREVIEW_TRIAL)
bool s_rgbPreview = false;
#endif
}

extern "C" esp_err_t __real_cam_config(const camera_config_t*, framesize_t, uint16_t);
extern "C" bool __real_ll_cam_dma_sizes(void*);

extern "C" esp_err_t __wrap_cam_config(const camera_config_t* config,
                                      framesize_t size, uint16_t sensorPid) {
  // On this SDK the 16 MHz path uses direct PSRAM DMA. Leave it and RGB alone.
  s_internalJpeg = config && config->pixel_format == PIXFORMAT_JPEG &&
                  config->xclk_freq_hz != 16000000;
#if defined(ORIGAMI_RGB_PREVIEW_TRIAL)
  s_rgbPreview=config && config->pixel_format==PIXFORMAT_RGB565 &&
      config->xclk_freq_hz==24000000 && size==FRAMESIZE_QVGA && sensorPid==0x3660;
#endif
  const esp_err_t result = __real_cam_config(config, size, sensorPid);
  s_internalJpeg = false;
#if defined(ORIGAMI_RGB_PREVIEW_TRIAL)
  s_rgbPreview=false;
#endif
  return result;
}

extern "C" bool __wrap_ll_cam_dma_sizes(void* camera) {
  const bool ok = __real_ll_cam_dma_sizes(camera);
  if (!ok || !camera) return ok;

#if defined(ORIGAMI_RGB_PREVIEW_TRIAL)
  if (s_rgbPreview) {
    DmaGeometry rgb;
    memcpy(&rgb,camera,sizeof(rgb));
    // Pinned S3 RGB565 QVGA geometry: 640 bytes per line, 153600 per frame.
    // The driver derives descriptor/frame-copy counts AFTER this wrapper.
    // Ten-line halves and two-line nodes divide both each other and the frame.
    // Keep two ping-pong halves; qualify interrupt/copy timing on the device.
    if (rgb.bytesPerItem==1 && rgb.bufferSize==30720 && rgb.chunkSize==15360 &&
        rgb.chunkCount==2 && rgb.nodeSize==3840) {
      const size_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
      if (largest>=13824u && largest<31744u) {
        rgb.bufferSize=12800;rgb.chunkSize=6400;rgb.nodeSize=1280;
        memcpy(camera,&rgb,sizeof(rgb));
        ESP_LOGW("camera-dma","RGB QVGA ring 30720 -> 12800, half=6400 node=1280 (largest=%u)",(unsigned)largest);
      }
    }
    return ok;
  }
#endif
  if (!s_internalJpeg) return ok;

  DmaGeometry dma;
  memcpy(&dma, camera, sizeof(dma));
  // Match only the bundled S3 JPEG layout. Driver code derives descriptor and
  // event-queue counts afterwards, so updating these together keeps its ring
  // consistent. Each transfer still copies 1 KB; only buffering depth changes.
  if (dma.bytesPerItem != 1 || dma.bufferSize != 16384 ||
      dma.chunkSize != 1024 || dma.chunkCount != 16 || dma.nodeSize != 1024)
    return ok;

  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  // Leave room for the small allocations performed before the ring allocation.
  if (largest < dma.bufferSize + 1024u) {
    // Network activity can leave >40 KiB free in blocks smaller than 8 KiB.
    // Use four complete 1 KiB chunks only when they fit with the same headroom.
    // This preserves descriptor geometry and a three-entry event queue. Do not
    // shrink further: buffering/overrun behavior needs physical qualification.
    dma.bufferSize = largest >= 5120u && largest < 9216u ? 4096u : 8192u;
    dma.chunkCount = dma.bufferSize / dma.chunkSize;
    memcpy(camera, &dma, sizeof(dma));
    // Called under HAL's I2C lock: use Serial/SDK logging, never SD telemetry.
    ESP_LOGW("camera-dma", "JPEG DMA ring 16384 -> %u bytes (largest=%u)",
             (unsigned)dma.bufferSize, (unsigned)largest);
  }
  return ok;
}
