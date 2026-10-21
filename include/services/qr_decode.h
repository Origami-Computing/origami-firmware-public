#pragma once

#include <stddef.h>
#include <stdint.h>

namespace services {
namespace qr {

using PreviewBlitFn = void (*)(uint16_t* mcuRgb565, int mcuW, int mcuH, int dx, int dy,
                               void* ctx);

/** UI-owned start/submit APIs; false if either queue or the task cannot start. */
bool ensureWorker();
const char* workerStartError();
bool workerReady();
void logDiagnostics();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Idle-only, one-shot worker shutdown acknowledgment delay (0..10000 ms).
bool delayNextWorkerStopForDevelopment(uint32_t milliseconds);
// One-shot allocation fault: 1=job queue, 2=result queue, 3=task. Idle only.
bool failNextWorkerStartForDevelopment(unsigned stage);
bool testStoppedSubmissionForDevelopment();
bool testWorkerDecodeForDevelopment();
#endif

/** True while a gray-frame scan is queued or running. */
bool scanBusy();

/** Stop the worker and release all QR decode buffers. */
bool shutdownWorker(uint32_t timeoutMs = 3000);

/**
 * Decode one JPEG frame on the caller's task (loopTask): optional MCU blit for
 * preview + internal grayscale buffer for QR. JPEGDecoder is not thread-safe —
 * all JPEG work must stay on this path.
 */
bool decodeJpegFrame(const uint8_t* jpeg, size_t len, PreviewBlitFn blit, void* ctx);

/** Fast full-resolution preview with only the central 320x240 converted to gray.
 * Skips grayscale work when the worker already owns a frame. */
bool decodeScannerJpeg(const uint8_t* jpeg, size_t len, PreviewBlitFn blit, void* ctx, bool buildGray);

/** Dimensions of the last successful grayscale decode. */
int lastFrameWidth();
int lastFrameHeight();

/**
 * Dump the last decoded gray frame to a PGM (P5) file. Used for the wifi
 * setup QR debug pipeline: save this side-by-side with the source JPEG so
 * we can confirm the gray buffer quirc sees actually matches the JPEG's
 * visible content (right brightness, right orientation, no byte-swap or
 * MCU-tile reordering bugs).
 *
 * Returns false if no frame has been decoded yet, the file can't be opened,
 * or the write is short. PGM format is a plain text header + raw 8-bit
 * grayscale pixels; opens directly in Preview / GIMP / any PGM viewer.
 */
bool dumpLastGrayPgm(const char* path);

/**
 * Queue the last gray frame for quirc. By default we run at the camera's
 * native QVGA (320×240) — no downscale, no detail lost from small QRs
 * (37+ modules at typical 15 cm hold distance is right at quirc's
 * detection threshold). A previous version capped at 280×210 because four
 * decode variants per job could pin core 0 for >5 s and trip the IDLE0
 * watchdog; with the variant count halved (normal + inverted only) and a
 * vTaskDelay(1) yield between them, full QVGA fits comfortably.
 */
bool submitForScan(int maxW = 320, int maxH = 240);

/**
 * Queue the last gray frame for quirc, but CENTER-CROPPED to cropW×cropH
 * instead of downscaled. Use this when the source frame is wider than what
 * quirc can usefully chew on AND you want to discard the outer (geometrically
 * distorted) region rather than blur it into the center via averaging.
 *
 * Rationale (wifi_setup_app): the camera on this board has a ~160° lens.
 * Capturing at VGA (640×480) and downscaling to 320×240 BLURS distorted
 * outer modules into the center; cropping the central 320×240 instead
 * gives quirc 2× pixels-per-module on the QR AND restricts it to the
 * lens's ~50° low-distortion zone where the 1:1:3:1:1 finder ratio still
 * survives Otsu binarisation.
 *
 * If the source gray buffer is already at or below cropW×cropH the function
 * falls back to submitting it as-is (no cropping, no downscaling).
 *
 * Returns false if the worker queue is full or memory allocation fails.
 */
bool submitForScanCenterCrop(int cropW = 320, int cropH = 240);

/** Non-blocking: returns true when a decoded QR payload is ready. */
bool pollPayload(char* payload, size_t payloadCap);

/** Blocking single-thread decode (tests / fallback). */
bool decodeFromJpeg(const uint8_t* jpeg, size_t len, char* payload, size_t payloadCap);

}  // namespace qr
}  // namespace services
