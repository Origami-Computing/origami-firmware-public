# Camera photo upload handoff

The Camera opened from Home and the main menu uses `cine_camera::Session`.
Its still-capture path saved the JPEG and metadata but never called
`photos::enqueue` or `photos::markCaptured`. The legacy Camera reached by the
`camera open` serial command does both, so that command alone cannot reproduce
the main-menu failure.

On official v1.0.12, a main-menu capture produced a valid 2048 × 1536 JPEG.
It rendered in Photos and passed CRC-checked retrieval from SD, while the queue
remained empty. Selecting Upload in Photos then delivered that same image with
HTTP 200 and a valid upload receipt; it appeared in the web timeline.

The fix records capture time when the fresh frame is acquired and queues only
successfully saved JPEGs, after returning the sensor frame. Existing peripheral
reservations defer network transfer until Camera exits or Photos starts syncing.
Queue failure preserves the local JPEG and displays `SAVED / NOT QUEUED`; the
user can retry from Photos. Metadata failure does not block an otherwise valid
photo from uploading.

## Regression checks

`tools/test_cine_service.py` compiles the real capture service with faultable
hardware/storage adapters. The new assertion fails against the original code.
It covers one enqueue per saved capture, capture timestamp propagation, frame
release before enqueue, no enqueue after a failed save, local retention on queue
failure, and unknown capture time. Existing freshness, metadata and lifecycle
checks remain in place.

## Patched device validation — November 17, 2026

The production-policy build passed (5,822,864-byte application image), was
flashed to the verified active slot, passed digest verification and booted.
Partition, NVS and OTA-selection metadata were unchanged during installation.

- A fresh capture through Home's Camera produced `IMG_00004.jpg`, with
  `saved=1 queued=1`. The queue was pending while Camera held the peripheral
  reservation.
- CRC-checked retrieval verified the 125,259-byte, 2048 × 1536 JPEG and the
  persisted manifest: matching photo path, nonzero capture time and
  `deleteAfterUpload=false`.
- Leaving Camera automatically started the upload. It completed with HTTP 200,
  passed the existing response-receipt validation and wrote a local delivery
  receipt. No manual Upload action was used for this capture.
- Photos rendered the same file after upload (`ready=1`, `ok=1`), confirming
  that local viewing remains available.
- The browser displayed the matching image in both Timeline and Library, using
  the same media identifier. The full preview matched the retrieved JPEG and
  showed capture time separately from save time.

Passed host checks: Cine service, photo storage, photo multipart timestamp,
Cine interactions, Cine overlay rendering, architecture dependencies and their
seven regression tests, tracked-secret scan, and the production firmware build.
The behavioral C++ tests ran with AddressSanitizer/UndefinedBehaviorSanitizer.

Private device identities, photos, logs and recovery images stay in ignored
`.origami/` files. This verifies one complete patched capture on the connected
device; it is not a long-duration network stress test. Older photos missed by
the original handoff can be uploaded using Photos' existing Upload action.
