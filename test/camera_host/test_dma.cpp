#include "camera_test_sdk.h"

FakeState fake;
extern "C" bool __wrap_ll_cam_dma_sizes(void*);
extern "C" esp_err_t __wrap_cam_config(const camera_config_t*, framesize_t, uint16_t);

// The SDK's geometry prefix followed by fields the wrapper must not touch.
struct DriverState {
  uint32_t bytes, total, chunk, chunks, node;
  uint32_t descriptorCount, copyCount, sentinel;
} driver;
static bool sizeSuccess = true;
static bool unusualGeometry = false;
static esp_err_t configResult = ESP_OK;

extern "C" bool __real_ll_cam_dma_sizes(void* object) {
  auto* d = static_cast<DriverState*>(object);
  *d = {1, 16384, 1024, 16, 1024, 0, 0, 0x12345678};
  if (unusualGeometry) d->node = 512;
  return sizeSuccess;
}

extern "C" esp_err_t __real_cam_config(const camera_config_t*, framesize_t, uint16_t) {
  if (!__wrap_ll_cam_dma_sizes(&driver)) return ESP_FAIL;
  // As in cam_dma_config: these are derived AFTER ll_cam_dma_sizes returns.
  driver.descriptorCount = driver.total / driver.node;
  driver.copyCount = (640u * 480u / 5u) / driver.chunk;
  return configResult;
}

int main() {
  camera_config_t config{};
  config.pixel_format = PIXFORMAT_JPEG;
  config.xclk_freq_hz = 24000000;
  const auto run = [&]() { return __wrap_cam_config(&config, FRAMESIZE_VGA, 0x5640); };
  assert(run() == ESP_OK && driver.total == 16384 && driver.chunks == 16);
  // Both exact hardware failures, plus headroom boundary cases.
  for (size_t largest : {16372u, 12788u, 8192u, 17407u}) {
    fake.dmaLargest = largest;
    assert(run() == ESP_OK);
    assert(driver.total == 8192 && driver.chunks == 8);
    assert(driver.chunk == 1024 && driver.node == 1024);
    assert(driver.descriptorCount == 8 && driver.copyCount == 60);
    assert(driver.sentinel == 0x12345678);
  }
  fake.dmaLargest = 17408;
  assert(run() == ESP_OK && driver.total == 16384);
  fake.dmaLargest = 12788;
  config.pixel_format = 0; // RGB delegates unchanged.
  assert(run() == ESP_OK && driver.total == 16384);
  config.pixel_format = PIXFORMAT_JPEG;
  config.xclk_freq_hz = 16000000; // Direct PSRAM DMA delegates unchanged.
  assert(run() == ESP_OK && driver.total == 16384);
  config.xclk_freq_hz = 24000000;
  unusualGeometry = true;
  assert(run() == ESP_OK && driver.total == 16384 && driver.node == 512);
  unusualGeometry = false;
  sizeSuccess = false;
  assert(run() == ESP_FAIL && driver.total == 16384);
  sizeSuccess = true;
  configResult = ESP_ERR_NO_MEM;
  assert(run() == ESP_ERR_NO_MEM);
  // Scope cleared even after driver failure: unrelated calls aren't modified.
  assert(__wrap_ll_cam_dma_sizes(&driver) && driver.total == 16384);
  puts("PASS camera DMA geometry, fragmented heaps, format isolation, error cleanup");
}
