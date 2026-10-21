#include "services/qr_decode.h"

#include <Arduino.h>
#include <JPEGDecoder.h>
#include <JPEGDEC.h>
#include <new>
#include "hal/hal_input.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <atomic>

#include "hal/hal_tasks.h"
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include "qrcodegen.h"
#include <esp_task_wdt.h>
#endif

extern "C" {
#include "quirc.h"
// quirc_internal.h gives us access to num_capstones (finder-pattern
// candidates) and num_grids (fully identified QRs). Public quirc_count()
// only returns num_grids, which collapses two very different failure
// modes — "no finder patterns found at all" and "finder patterns found
// but couldn't be grouped into a QR" — into the same "0" output.
#include "quirc_internal.h"
}

namespace services {
namespace qr {
namespace {

constexpr size_t kPayloadQueueLen = 256;
constexpr size_t kWorkerStack     = 8192;
// Sized for VGA (640×480) gray-buffer capture. quirc itself still chews on at
// most 320×240, but the *source* gray buffer is now VGA so we can center-crop
// the low-distortion zone (see submitForScanCenterCrop). 307 KB in PSRAM —
// ~20 % of the 1.4 MB freeContig we see at app entry.
constexpr size_t kMaxGrayBytes    = 640u * 480u;
// Run quirc at QVGA — no detail lost from the small (37-module) provisioning
// QR at typical 15 cm hold distance. 320×240 only ever tripped the task
// watchdog back when we ran 4 decode variants per job (normal/inverted ×
// flipped/non-flipped). The camera HAL now normalizes module orientation before
// JPEG encode, so we only run the 2 variants (normal + inverted) with a
// vTaskDelay(1) yield between them, and the worst-case CPU time per frame
// is ~1.7 s — well under the 5 s IDLE0 watchdog.
constexpr int      kQuircMaxW     = 320;
constexpr int      kQuircMaxH     = 240;

static JPEGDEC* s_scannerDecoder = nullptr;
static uint8_t* s_gray    = nullptr;
static size_t   s_grayLen = 0;
static int      s_grayW   = 0;
static int      s_grayH   = 0;

// Large symbol/payload/Reed-Solomon scratch belongs in PSRAM, not in the
// scarce contiguous internal-memory task stack. Released only after worker exit.
static quirc_code* s_code = nullptr;
static quirc_data* s_data = nullptr;
static void* s_decodeWorkspace = nullptr;
static void releaseDecodeScratch() {
  heap_caps_free(s_code); s_code=nullptr;
  heap_caps_free(s_data); s_data=nullptr;
  heap_caps_free(s_decodeWorkspace); s_decodeWorkspace=nullptr;
}
static bool ensureDecodeScratch() {
  if(!s_code) s_code=(quirc_code*)heap_caps_malloc(sizeof(quirc_code),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!s_data) s_data=(quirc_data*)heap_caps_malloc(sizeof(quirc_data),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!s_decodeWorkspace) s_decodeWorkspace=heap_caps_malloc(quirc_decode_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(s_code && s_data && s_decodeWorkspace) return true;
  releaseDecodeScratch(); return false;
}
static quirc*     s_quirc = nullptr;
static int        s_qrW   = 0;
static int        s_qrH   = 0;

static QueueHandle_t s_jobQ    = nullptr;
static QueueHandle_t s_resultQ = nullptr;
static std::atomic<TaskHandle_t> s_worker{nullptr};
static std::atomic<bool> s_workerBusy{false};
static std::atomic<bool> s_stopRequested{false};
static std::atomic<uint32_t> s_processed{0}, s_decoded{0}, s_minStack{0};
static uint32_t s_submitted = 0;
static uint32_t s_lastDecodeLogMs = 0;
static unsigned s_failStartStage = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static std::atomic<uint32_t> s_stopDelayMs{0};
#endif

// Guard decoder operations, including deferred UI-task service cleanup, against
// overlapping queue creation, frame submission, or development diagnostics.
// This is a non-blocking ownership gate, NOT a critical section around decoding.
static std::atomic_flag s_apiOwned = ATOMIC_FLAG_INIT;
struct ApiGuard {
  bool held = !s_apiOwned.test_and_set();
  ~ApiGuard() { if (held) s_apiOwned.clear(); }
};

struct GrayJob {
  uint8_t* data;
  int      w;
  int      h;
  size_t   len;
};

// Computes Rec.601 luma from one RGB565 pixel value. `c` MUST be in native
// little-endian RGB565 (R in the top 5 bits, B in the low 5 bits). The MCU
// loop in decodeJpegFrame() reads JpegDec.pImage which is byte-swapped (the
// format the TFT panel wants over SPI), so callers there must swap before
// passing the word in — see decodeJpegFrame() for the swap site. Feeding
// the swapped value straight in produces nonsense luma (a pure-red pixel
// ends up reading as dark blue), destroys QR module contrast, and was the
// reason quirc consistently saw cand=0/0 despite the JPEGs themselves
// being decodable in any online QR reader.
static inline uint8_t lumaFromRgb565(uint16_t c) {
  const uint8_t r = (uint8_t)((c >> 11) << 3);
  const uint8_t g = (uint8_t)(((c >> 5) & 0x3Fu) << 2);
  const uint8_t b = (uint8_t)((c & 0x1Fu) << 3);
  return (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
}

static bool ensureGrayBuffer(size_t need) {
  if (s_gray && s_grayLen >= need) return true;
  if (s_gray) {
    heap_caps_free(s_gray);
    s_gray = nullptr;
    s_grayLen = 0;
  }
  if (need > kMaxGrayBytes) return false;
  s_gray = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_gray) s_gray = (uint8_t*)malloc(need);
  if (!s_gray) return false;
  s_grayLen = need;
  return true;
}

static bool ensureQuirc(int w, int h) {
  if(!ensureDecodeScratch()) return false;
  if (!s_quirc) {
    s_quirc = quirc_new();
    if (!s_quirc) return false;
    s_qrW = s_qrH = 0;
  }
  if (w != s_qrW || h != s_qrH) {
    if (quirc_resize(s_quirc, w, h) < 0) return false;
    s_qrW = w;
    s_qrH = h;
  }
  return true;
}

static void histogramEqualize(uint8_t* gray, size_t n) {
  uint32_t hist[256] = {0};
  for (size_t i = 0; i < n; i++) hist[gray[i]]++;

  uint32_t cdf[256];
  cdf[0] = hist[0];
  for (int i = 1; i < 256; i++) cdf[i] = cdf[i - 1] + hist[i];

  uint32_t cdfMin = 0;
  for (int i = 0; i < 256; i++) {
    if (cdf[i]) {
      cdfMin = cdf[i];
      break;
    }
  }
  if (cdfMin == 0 || n <= cdfMin) return;

  const uint32_t denom = (uint32_t)(n - cdfMin);
  for (size_t i = 0; i < n; i++) {
    gray[i] = (uint8_t)(((cdf[gray[i]] - cdfMin) * 255u) / denom);
  }
}

static void enhanceContrast(uint8_t* gray, size_t n) {
  uint32_t hist[256] = {};
  for (size_t i = 0; i < n; i++) hist[gray[i]]++;

  // Percentile clip: 5th → black, 70th → white.
  // Min-max anchored to lone dark vignette pixels (min=0) and
  // clipped highlights (max=246) gives a ~1.04× stretch — useless.
  // Percentile clip ignores those outliers and gives quirc a
  // full-range binary where module edges are actually at 0/255.
  const uint32_t lo_target = (uint32_t)(n * 5  / 100);
  const uint32_t hi_target = (uint32_t)(n * 70 / 100);

  uint8_t  lo = 0, hi = 255;
  uint32_t cum = 0;
  for (int v = 0; v < 256; v++) {
    cum += hist[v];
    if (lo == 0 && cum >= lo_target && v > 0) lo = (uint8_t)v;
    if (cum >= hi_target) { hi = (uint8_t)v; break; }
  }

  if (hi <= lo + 8) { histogramEqualize(gray, n); return; }

  const uint16_t range = (uint16_t)(hi - lo);
  for (size_t i = 0; i < n; i++) {
    if      (gray[i] <= lo) gray[i] = 0;
    else if (gray[i] >= hi) gray[i] = 255;
    else                    gray[i] = (uint8_t)(((uint16_t)(gray[i] - lo) * 255u) / range);
  }
}

// Out params: capstones = number of finder-pattern candidates quirc detected
// (3 means a full QR was located), grids = number of complete QR codes
// successfully extracted (== quirc_count). -1 in either means "quirc init
// failed before that stage". Splitting these distinguishes "no finder
// patterns" (lighting/blur/size problem) from "finder patterns OK but
// extraction failed" (perspective distortion, lens warp, severe data noise).
static bool quircDecodeGray(const uint8_t* gray, int w, int h, char* payload,
                             size_t payloadCap, bool invert,
                             int* outCapstones, int* outGrids) {
  if (outCapstones) *outCapstones = -1;
  if (outGrids)     *outGrids     = -1;
  const size_t need = (size_t)w * (size_t)h;
  if (!ensureQuirc(w, h)) return false;

  uint8_t* image = quirc_begin(s_quirc, nullptr, nullptr);
  for (size_t i = 0; i < need; i++) {
    const uint8_t v = gray[i];
    image[i] = invert ? (uint8_t)(255u - v) : v;
  }
  quirc_end(s_quirc);

  if (outCapstones) *outCapstones = s_quirc->num_capstones;
  const int count = quirc_count(s_quirc);
  if (outGrids) *outGrids = count;
  if (count <= 0) return false;

  quirc_code& code=*s_code;
  quirc_data& data=*s_data;
  for (int i = 0; i < count; i++) {
    quirc_extract(s_quirc, i, &code);
    if (quirc_decode_with_workspace(&code, &data, s_decodeWorkspace, quirc_decode_workspace_size()) != QUIRC_SUCCESS || data.payload_len == 0) continue;

    size_t n = data.payload_len;
    if (n >= payloadCap || memchr(data.payload, '\0', n)) continue;
    memcpy(payload, data.payload, n);
    payload[n] = '\0';
    return true;
  }
  return false;
}

// Cooperatively yield between expensive quirc passes. Each pass can spend
// hundreds of ms in finder_scan / identify when the frame is noisy; without
// a yield between them, core 0's IDLE task never runs and task_wdt fires
// (was the cause of the original abort in finder_scan@identify.c:548).
// Yielding for ≥ 1 tick guarantees IDLE0 gets CPU time and refreshes the
// system IDLE watchdog naturally — no need to subscribe this worker to
// task_wdt. (An earlier attempt did that, and it back-fired: the worker
// spends nearly all its life blocked on xQueueReceive with portMAX_DELAY,
// which can't reset the WDT and triggered task_wdt on `qr_scan` itself
// after long idle periods.)
static inline void yieldBetweenPasses() {
  vTaskDelay(1);
}

struct DecodeDiag {
  uint8_t  grayMinPre;     // gray buffer min before enhanceContrast
  uint8_t  grayMaxPre;     // gray buffer max before enhanceContrast
  uint16_t grayMeanPre;    // gray buffer mean before enhanceContrast
  int      capsNormal;     // num_capstones after normal-polarity pass (-1 = init fail)
  int      gridsNormal;    // quirc_count after normal-polarity pass
  int      capsInverted;   // num_capstones after inverted pass (-1 = not run)
  int      gridsInverted;  // quirc_count after inverted pass
};

static void grayHistogramStats(const uint8_t* gray, size_t n,
                                uint8_t* outMin, uint8_t* outMax, uint16_t* outMean) {
  if (n == 0) {
    if (outMin)  *outMin  = 0;
    if (outMax)  *outMax  = 0;
    if (outMean) *outMean = 0;
    return;
  }
  uint8_t  lo  = 255;
  uint8_t  hi  = 0;
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t v = gray[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    sum += v;
  }
  if (outMin)  *outMin  = lo;
  if (outMax)  *outMax  = hi;
  if (outMean) *outMean = (uint16_t)(sum / (uint64_t)n);
}

static bool tryDecodeVariants(uint8_t* gray, int w, int h, char* payload, size_t payloadCap,
                              DecodeDiag* diag) {
  int caps = -1;
  int grids = -1;
  if (quircDecodeGray(gray, w, h, payload, payloadCap, false, &caps, &grids)) {
    if (diag) { diag->capsNormal = caps; diag->gridsNormal = grids; }
    return true;
  }
  if (diag) { diag->capsNormal = caps; diag->gridsNormal = grids; }
  yieldBetweenPasses();
  caps = -1;
  grids = -1;
  if (quircDecodeGray(gray, w, h, payload, payloadCap, true, &caps, &grids)) {
    if (diag) { diag->capsInverted = caps; diag->gridsInverted = grids; }
    return true;
  }
  if (diag) { diag->capsInverted = caps; diag->gridsInverted = grids; }
  return false;
}

static bool decodeGrayToPayload(uint8_t* gray, int w, int h, char* payload, size_t payloadCap,
                                DecodeDiag* diag) {
  const size_t need = (size_t)w * (size_t)h;

  // Capture the gray-buffer histogram *before* contrast normalisation — that's
  // the raw camera signal. Post-contrast everything is stretched to 0..255 by
  // definition so it's not useful for diagnosing under/overexposure.
  if (diag) {
    grayHistogramStats(gray, need, &diag->grayMinPre, &diag->grayMaxPre, &diag->grayMeanPre);
    diag->capsNormal    = -1;
    diag->gridsNormal   = -1;
    diag->capsInverted  = -1;
    diag->gridsInverted = -1;
  }

  enhanceContrast(gray, need);
  // Only normal + inverted variants. The flipped-horizontal pass we used to
  // run was a hedge for sensor mirroring; the camera HAL now normalizes module
  // orientation before JPEG encode, so the gray buffer that lands here is
  // already in quirc's expected orientation and we save half the worst-case
  // decode time — which is what tipped us over the task watchdog at 320×240.
  return tryDecodeVariants(gray, w, h, payload, payloadCap, diag);
}

static void workerTask(void*) {
  GrayJob job;
  char    payload[kPayloadQueueLen];
  for (;;) {
    if (s_stopRequested) break;
    if (xQueueReceive(s_jobQ, &job, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    s_workerBusy = true;
    DecodeDiag diag = {};
    bool ok = false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if(job.w==0 && job.h==0) {
      // Maximum-version synthetic symbol: tests decoder/ECC stack independently
      // of the finder, which cannot reliably locate one-pixel camera modules.
      ok=quirc_decode_with_workspace((const quirc_code*)job.data,s_data,s_decodeWorkspace,
          quirc_decode_workspace_size())==QUIRC_SUCCESS && s_data->payload_len<(int)sizeof(payload);
      if(ok) { memcpy(payload,s_data->payload,s_data->payload_len); payload[s_data->payload_len]=0; }
    } else
#endif
    ok = decodeGrayToPayload(job.data, job.w, job.h, payload, sizeof(payload), &diag);
    ++s_processed;
    s_minStack = uxTaskGetStackHighWaterMark(nullptr);
    if (!ok && millis() - s_lastDecodeLogMs >= 3000) {
      s_lastDecodeLogMs = millis();
      // Diagnostic line: raw camera stats + a two-stage view of quirc's
      // detection pipeline. Reading the numbers (caps = finder patterns found,
      // grids = full QRs extracted; format is normal/inverted polarity):
      //   min/max/mean very narrow (e.g. 0..15 or 240..255)
      //                       → exposure problem (too dark / blown out)
      //   caps=0/0            → quirc isn't even seeing finder patterns. QR
      //                          too small in frame, severely blurred, or
      //                          something is wrong with the gray pipeline.
      //   caps>=3, grids=0    → finder patterns found, full QR extraction
      //                          failed. Usually severe lens distortion or
      //                          missing alignment pattern; preprocessing
      //                          (denoise, undistort) is the next move.
      //   caps>0 but <3       → partial occlusion / clipping — one of the
      //                          finder patterns is off-frame or hidden by
      //                          glare. Reframe.
      Serial.printf("[qr] no code (%dx%d) min=%u max=%u mean=%u caps=%d/%d grids=%d/%d\n",
                    job.w, job.h,
                    (unsigned)diag.grayMinPre, (unsigned)diag.grayMaxPre,
                    (unsigned)diag.grayMeanPre,
                    diag.capsNormal, diag.capsInverted,
                    diag.gridsNormal, diag.gridsInverted);
    } else if (ok) {
      ++s_decoded;
      Serial.println("[qr] decoded payload (contents withheld)");
    }
    heap_caps_free(job.data);
    if (ok) {
      (void)xQueueSend(s_resultQ, payload, 0);
    }
    s_workerBusy = false;
    // Give IDLE0 a slice between jobs even if the next one is already queued.
    vTaskDelay(1);
  }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  // Bounded bench fault; yield normally so this tests delayed acknowledgment,
  // not a wedged CPU or disabled watchdog.
  const auto delayMs = s_stopDelayMs.exchange(0);
  if (delayMs) vTaskDelay(pdMS_TO_TICKS(delayMs));
#endif
  s_workerBusy = false;
  s_worker = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool workerReady() {
  return s_worker.load() != nullptr && !s_stopRequested.load();
}

static const char* s_startError="Decoder busy or stopping";
const char* workerStartError(){return s_startError;}
bool ensureWorker() {
  s_startError="Decoder busy or stopping";
  ApiGuard guard;
  if (!guard.held || s_stopRequested) return false;
  if (s_worker) return true;
  const unsigned fault = s_failStartStage;
  s_failStartStage = 0;
  s_submitted = 0;
  s_processed = s_decoded = s_minStack = 0;
  const char* failure = nullptr;
  if (!s_jobQ && fault != 1) s_jobQ = xQueueCreate(1, sizeof(GrayJob));
  if (!s_jobQ) failure = "job queue allocation";
  if (!failure && !s_resultQ && fault != 2) s_resultQ = xQueueCreate(1, kPayloadQueueLen);
  if (!failure && !s_resultQ) failure = "result queue allocation";
  s_stopRequested = false;
  if (!failure && !ensureDecodeScratch()) failure="PSRAM decode scratch allocation";
  if (!failure && fault != 3) s_worker = hal::tasks::spawn(hal::tasks::Id::QrScan, workerTask);
  if (!failure && !s_worker) failure = "decoder task allocation";
  if (failure) {
    s_startError=failure;
    releaseDecodeScratch();
    // There is no worker and no submitted frame: partial initialization owns
    // only the queues. Leave nothing that could masquerade as a busy scanner.
    if (s_jobQ) { vQueueDelete(s_jobQ); s_jobQ = nullptr; }
    if (s_resultQ) { vQueueDelete(s_resultQ); s_resultQ = nullptr; }
    Serial.printf("[qr] start failed reason=%s injected=%u stack=%u free=%u largest=%u\n",
                  failure, fault, (unsigned)kWorkerStack,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return false;
  }
  Serial.println("[qr] start ready worker=1 queues=2");
  return true;
}

void logDiagnostics() {
  ApiGuard guard;
  if (!guard.held) return;
  Serial.printf("[qr] progress ready=%u stopping=%u queued=%u submitted=%lu processed=%lu decoded=%lu minStack=%lu\n",
                workerReady(), (unsigned)s_stopRequested.load(),
                s_jobQ ? (unsigned)uxQueueMessagesWaiting(s_jobQ) : 0,
                (unsigned long)s_submitted, (unsigned long)s_processed.load(),
                (unsigned long)s_decoded.load(), (unsigned long)s_minStack.load());
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool testWorkerDecodeForDevelopment() {
  if(s_worker || s_stopRequested || !ensureWorker()) return false;
  bool passed=true;
  for(int version : {4,10,40}) {
    esp_task_wdt_reset();
    auto* temp=(uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    auto* code=(uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    auto* gray=(uint8_t*)heap_caps_malloc(320u*240u,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    char expected[221],actual[256];
    const int length=version==4?20:200;
    for(int i=0;i<length;++i) expected[i]='a'+i%26;
    expected[length]=0;
    bool ok=temp && code && gray && qrcodegen_encodeText(expected,temp,code,qrcodegen_Ecc_MEDIUM,
        version,version,qrcodegen_Mask_AUTO,false);
    if(ok) {
      memset(gray,255,320u*240u);
      const int modules=qrcodegen_getSize(code), scale=224/modules;
      const int x0=(320-modules*scale)/2,y0=(240-modules*scale)/2;
      for(int y=0;y<modules;++y) for(int x=0;x<modules;++x) if(qrcodegen_getModule(code,x,y))
        for(int dy=0;dy<scale;++dy) memset(gray+(y0+y*scale+dy)*320+x0+x*scale,0,scale);
      GrayJob job{gray,320,240,320u*240u};
      if(version==40) {
        auto* symbol=(quirc_code*)gray;
        memset(symbol,0,sizeof(*symbol)); symbol->size=modules;
        for(int y=0;y<modules;++y) for(int x=0;x<modules;++x) if(qrcodegen_getModule(code,x,y)) {
          const int bit=y*modules+x; symbol->cell_bitmap[bit/8]|=1u<<(bit%8);
        }
        job.w=job.h=0; job.len=sizeof(*symbol);
      }
      ApiGuard guard;
      ok=guard.held && workerReady() && s_jobQ && xQueueSend(s_jobQ,&job,0)==pdTRUE;
      if(ok) { gray=nullptr; ++s_submitted; }
    }
    heap_caps_free(gray); heap_caps_free(temp); heap_caps_free(code);
    bool decoded=false;
    const uint32_t started=millis();
    while(ok && millis()-started<10000u) {
      esp_task_wdt_reset(); // Bounded development test wait; production decoding stays asynchronous.
      if(pollPayload(actual,sizeof(actual))) { decoded=!strcmp(actual,expected); break; }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    Serial.printf("[qr] selftest version=%d payloadMatch=%u minStack=%lu scratch=%u\n",version,decoded,
        (unsigned long)s_minStack.load(),(unsigned)(sizeof(quirc_code)+sizeof(quirc_data)+quirc_decode_workspace_size()));
    passed &= decoded;
    if(!decoded) break;
  }
  logDiagnostics();
  return shutdownWorker() && passed;
}

bool delayNextWorkerStopForDevelopment(uint32_t milliseconds) {
  ApiGuard guard;
  if (!guard.held || s_worker || s_stopRequested || milliseconds > 10000) return false;
  s_stopDelayMs = milliseconds;
  return true;
}
bool failNextWorkerStartForDevelopment(unsigned stage) {
  ApiGuard guard;
  if (!guard.held || s_worker || s_stopRequested || stage > 3) return false;
  s_failStartStage = stage;
  return true;
}

bool testStoppedSubmissionForDevelopment() {
  {
    ApiGuard guard;
    if (!guard.held || s_worker || s_stopRequested || s_jobQ || s_resultQ || s_gray) return false;
    // A valid input frame ensures the test reaches the worker guard, rather
    // than passing trivially because no camera frame exists.
    if (!ensureGrayBuffer(32u * 32u)) return false;
    memset(s_gray, 255, 32u * 32u);
    s_grayW = s_grayH = 32;
  }
  const bool scaledRejected = !submitForScan(16, 16);
  const bool croppedRejected = !submitForScanCenterCrop(16, 16);
  const bool empty = !scanBusy() && !workerReady();
  const bool cleaned = shutdownWorker();
  Serial.printf("[qr] guard-test scaledRejected=%u croppedRejected=%u empty=%u cleaned=%u\n",
                scaledRejected, croppedRejected, empty, cleaned);
  return scaledRejected && croppedRejected && empty && cleaned;
}
#endif

bool scanBusy() {
  ApiGuard guard;
  if (!guard.held) return true;
  if (!s_jobQ) return false;
  return s_workerBusy || uxQueueMessagesWaiting(s_jobQ) > 0;
}

bool shutdownWorker(uint32_t timeoutMs) {
  ApiGuard guard;
  if (!guard.held) return false;
  if (!s_worker && !s_jobQ && !s_resultQ && !s_quirc && !s_gray && !s_code && !s_scannerDecoder) return true;
  s_stopRequested = true;
  const uint32_t startedMs = millis();
  while (s_worker && (uint32_t)(millis() - startedMs) < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s_worker) {
    if (timeoutMs) Serial.println("[qr] worker shutdown timed out");
    return false;
  }

  if (s_jobQ) {
    GrayJob pending;
    while (xQueueReceive(s_jobQ, &pending, 0) == pdTRUE) {
      heap_caps_free(pending.data);
    }
    vQueueDelete(s_jobQ);
    s_jobQ = nullptr;
  }
  if (s_resultQ) {
    vQueueDelete(s_resultQ);
    s_resultQ = nullptr;
  }
  releaseDecodeScratch();
  if (s_quirc) {
    quirc_destroy(s_quirc);
    s_quirc = nullptr;
  }
  if (s_gray) {
    heap_caps_free(s_gray);
    s_gray = nullptr;
  }
  if (s_scannerDecoder) {
    s_scannerDecoder->~JPEGDEC();
    heap_caps_free(s_scannerDecoder);
    s_scannerDecoder = nullptr;
  }
  s_grayLen = 0;
  s_grayW = 0;
  s_grayH = 0;
  s_qrW = 0;
  s_qrH = 0;
  s_stopRequested = false;
  Serial.println("[qr] worker and buffers released");
  return true;
}

bool decodeJpegFrame(const uint8_t* jpeg, size_t len, PreviewBlitFn blit, void* ctx) {
  ApiGuard guard;
  if (!guard.held || s_stopRequested) return false;
  if (!jpeg || len == 0) return false;
  if (!JpegDec.decodeArray(const_cast<uint8_t*>(jpeg), len)) return false;

  const int w = JpegDec.width;
  const int h = JpegDec.height;
  if (w <= 0 || h <= 0 || w > 640 || h > 480) return false;

  const size_t need = (size_t)w * (size_t)h;
  if (!ensureGrayBuffer(need)) return false;
  memset(s_gray, 0, need);
  s_grayW = w;
  s_grayH = h;

  const uint16_t mcuW = JpegDec.MCUWidth;
  const uint16_t mcuH = JpegDec.MCUHeight;
  while (JpegDec.readSwappedBytes()) {
    uint16_t* pImg = (uint16_t*)JpegDec.pImage;
    const int   dx  = (int)(JpegDec.MCUx * mcuW);
    const int   dy  = (int)(JpegDec.MCUy * mcuH);
    if (blit) blit(pImg, (int)mcuW, (int)mcuH, dx, dy, ctx);

    for (uint16_t py = 0; py < mcuH; py++) {
      for (uint16_t px = 0; px < mcuW; px++) {
        const int x = dx + (int)px;
        const int y = dy + (int)py;
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        // pImg comes from JpegDec.readSwappedBytes() — bytes are in big-endian
        // order so the TFT push works directly. We must un-swap before luma
        // extraction, otherwise R/G/B bits land in the wrong slots and quirc
        // sees garbage (see lumaFromRgb565 doc comment).
        const uint16_t swapped = pImg[py * mcuW + px];
        const uint16_t native  = (uint16_t)((swapped >> 8) | (swapped << 8));
        s_gray[(size_t)y * (size_t)w + (size_t)x] = lumaFromRgb565(native);
      }
    }
  }
  return true;
}

// JPEGDEC is also used by the camera app. Keep native pixels for QR modules;
// only luminance conversion is cropped, never downsample the QR input.
struct ScannerDecode {
  PreviewBlitFn blit; void* ctx;
  int x, y, w, h; bool gray;
  uint32_t serviced;
};
static int scannerJpegBlock(JPEGDRAW* block) {
  auto& c = *static_cast<ScannerDecode*>(block->pUser);
  const int width = block->iWidthUsed;
  if (c.gray) {
    const int x0 = max(c.x, block->x), x1 = min(c.x+c.w, block->x+width);
    const int y0 = max(c.y, block->y), y1 = min(c.y+c.h, block->y+block->iHeight);
    for (int y=y0; y<y1; ++y) {
      auto* src = block->pPixels+(y-block->y)*block->iWidth;
      auto* dst = s_gray+(y-c.y)*c.w;
      for (int x=x0; x<x1; ++x) {
        uint16_t v=src[x-block->x];
        dst[x-c.x]=lumaFromRgb565(uint16_t((v>>8)|(v<<8)));
      }
    }
  }
  if (c.blit) {
    if (width==block->iWidth) c.blit(block->pPixels,width,block->iHeight,block->x,block->y,c.ctx);
    else for(int y=0;y<block->iHeight;++y)
      c.blit(block->pPixels+y*block->iWidth,width,1,block->x,block->y+y,c.ctx);
  }
  const uint32_t now=millis();
  if(now-c.serviced>=8){hal::input::service(now);c.serviced=now;}
  return 1;
}
bool decodeScannerJpeg(const uint8_t* jpeg,size_t len,PreviewBlitFn blit,void* ctx,bool buildGray) {
  ApiGuard guard;
  if(!guard.held || s_stopRequested || !jpeg || !len) return false;
  s_grayW=s_grayH=0; // A failed or preview-only decode cannot submit stale pixels.
  if(!s_scannerDecoder){
    void* memory=heap_caps_malloc(sizeof(JPEGDEC),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!memory) memory=heap_caps_malloc(sizeof(JPEGDEC),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory) return false;
    s_scannerDecoder=new(memory) JPEGDEC;
  }
  auto& decoder=*s_scannerDecoder;
  if(!decoder.openRAM(const_cast<uint8_t*>(jpeg),len,scannerJpegBlock)) return false;
  const int fw=decoder.getWidth(),fh=decoder.getHeight();
  if(fw<320 || fh<240 || fw>640 || fh>480){decoder.close();return false;}
  if(buildGray && !ensureGrayBuffer(320u*240u)){decoder.close();return false;}
  ScannerDecode context{blit,ctx,(fw-320)/2,(fh-240)/2,320,240,buildGray,millis()};
  decoder.setUserPointer(&context);
  decoder.setPixelType(RGB565_BIG_ENDIAN);
  const bool ok=decoder.decode(0,0,0)!=0;
  decoder.close();
  if(ok && buildGray){s_grayW=320;s_grayH=240;}
  return ok;
}

static bool downscaleGray(const uint8_t* src, int sw, int sh, int dw, int dh, uint8_t** out) {
  if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;

  const size_t len = (size_t)dw * (size_t)dh;
  uint8_t* dst = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!dst) dst = (uint8_t*)malloc(len);
  if (!dst) return false;

  if(sw==dw && sh==dh){memcpy(dst,src,len);*out=dst;return true;}

  for (int y = 0; y < dh; y++) {
    const int sy0 = y * sh / dh;
    const int sy1 = (y + 1) * sh / dh;
    for (int x = 0; x < dw; x++) {
      const int sx0 = x * sw / dw;
      const int sx1 = (x + 1) * sw / dw;
      uint32_t sum = 0;
      int      count = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
          sum += src[(size_t)sy * (size_t)sw + (size_t)sx];
          count++;
        }
      }
      dst[(size_t)y * (size_t)dw + (size_t)x] = count ? (uint8_t)(sum / (uint32_t)count) : 0;
    }
  }

  *out = dst;
  return true;
}

static bool submitScaledFrame(int maxW, int maxH) {
  if (!s_gray || s_grayW <= 0 || s_grayH <= 0) return false;
  if (maxW <= 0) maxW = kQuircMaxW;
  if (maxH <= 0) maxH = kQuircMaxH;
  if (maxW > kQuircMaxW) maxW = kQuircMaxW;
  if (maxH > kQuircMaxH) maxH = kQuircMaxH;

  if (!s_jobQ || s_workerBusy || uxQueueMessagesWaiting(s_jobQ)) return false;

  uint8_t* scaled = nullptr;
  int      dw = maxW;
  int      dh = maxH;
  if (s_grayW <= maxW && s_grayH <= maxH) {
    dw = s_grayW;
    dh = s_grayH;
  }
  if (!downscaleGray(s_gray, s_grayW, s_grayH, dw, dh, &scaled)) return false;

  GrayJob job = {scaled, dw, dh, (size_t)dw * (size_t)dh};
  if (xQueueSend(s_jobQ, &job, 0) != pdTRUE) {
    heap_caps_free(scaled);
    return false;
  }
  ++s_submitted;
  return true;
}

bool submitForScan(int maxW, int maxH) {
  ApiGuard guard;
  return guard.held && workerReady() && submitScaledFrame(maxW, maxH);
}

bool submitForScanCenterCrop(int cropW, int cropH) {
  ApiGuard guard;
  if (!guard.held || !workerReady()) return false;
  if (!s_gray || s_grayW <= 0 || s_grayH <= 0) return false;
  if (cropW <= 0) cropW = kQuircMaxW;
  if (cropH <= 0) cropH = kQuircMaxH;
  if (cropW > kQuircMaxW) cropW = kQuircMaxW;
  if (cropH > kQuircMaxH) cropH = kQuircMaxH;

  if (!s_jobQ || s_workerBusy || uxQueueMessagesWaiting(s_jobQ)) return false;

  // Source smaller than crop → submit whole frame (no point cropping less
  // than we have). Worker still does normal + inverted passes.
  if (s_grayW <= cropW && s_grayH <= cropH) {
    return submitScaledFrame(cropW, cropH);
  }

  const int srcW = s_grayW;
  const int srcH = s_grayH;
  // Pick the largest crop that fits in both src and the cropW×cropH budget.
  const int dw = (srcW < cropW) ? srcW : cropW;
  const int dh = (srcH < cropH) ? srcH : cropH;
  const int x0 = (srcW - dw) / 2;
  const int y0 = (srcH - dh) / 2;

  const size_t len = (size_t)dw * (size_t)dh;
  uint8_t* dst = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!dst) dst = (uint8_t*)malloc(len);
  if (!dst) return false;

  // Pure copy — no averaging, no scaling. The whole point of center-crop is
  // to KEEP the sharp module edges in the low-distortion central zone and
  // throw away the wide-angle outer region, not blur it into the result.
  for (int y = 0; y < dh; y++) {
    memcpy(dst + (size_t)y * (size_t)dw,
           s_gray + (size_t)(y + y0) * (size_t)srcW + (size_t)x0,
           (size_t)dw);
  }

  GrayJob job = {dst, dw, dh, len};
  if (xQueueSend(s_jobQ, &job, 0) != pdTRUE) {
    heap_caps_free(dst);
    return false;
  }
  ++s_submitted;
  return true;
}

int lastFrameWidth() { return s_grayW; }
int lastFrameHeight() { return s_grayH; }

bool dumpLastGrayPgm(const char* path) {
  ApiGuard guard;
  if (!guard.held) return false;
  if (!s_gray || s_grayW <= 0 || s_grayH <= 0 || !path) return false;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  char header[32];
  const int hlen = snprintf(header, sizeof(header), "P5\n%d %d\n255\n",
                            s_grayW, s_grayH);
  if (hlen <= 0 || (size_t)f.write((const uint8_t*)header, hlen) != (size_t)hlen) {
    f.close();
    return false;
  }
  const size_t bytes = (size_t)s_grayW * (size_t)s_grayH;
  const size_t wrote = f.write(s_gray, bytes);
  f.close();
  return wrote == bytes;
}

bool pollPayload(char* payload, size_t payloadCap) {
  ApiGuard guard;
  if (!guard.held || s_stopRequested) return false;
  if (!payload || payloadCap == 0 || !s_resultQ) return false;
  char tmp[kPayloadQueueLen];
  if (xQueueReceive(s_resultQ, tmp, 0) != pdTRUE) return false;
  strncpy(payload, tmp, payloadCap - 1);
  payload[payloadCap - 1] = '\0';
  return true;
}

bool decodeFromJpeg(const uint8_t* jpeg, size_t len, char* payload, size_t payloadCap) {
  if (!decodeJpegFrame(jpeg, len, nullptr, nullptr)) return false;
  ApiGuard guard;
  if (!guard.held || s_worker || s_stopRequested) return false;
  return decodeGrayToPayload(s_gray, s_grayW, s_grayH, payload, payloadCap, nullptr);
}

}  // namespace qr
}  // namespace services
