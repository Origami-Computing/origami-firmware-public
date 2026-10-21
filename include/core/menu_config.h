#pragma once

// Configuration that was historically defined inside `src/main_menu.cpp`.
// Credentials are provisioned on-device and stored in NVS; never embed them here.

#define WIFI_SSID       ""
#define WIFI_PASS       ""
#define BACKEND_URL     "http://192.168.1.163:3000"
#define BACKEND_URL_PROD "https://api.kami.so" // staging -> "https://kami2-staging.up.railway.app"
#define AUTH_TOKEN      ""

/** Outbound poll: GET BACKEND_URL + AUTOMATION_POLL_PATH (sdo ame auth as agent/upload queue). */
#ifndef AUTOMATION_POLL_PATH
#define AUTOMATION_POLL_PATH "/api/device/automations"
#endif

// ── Device telemetry (logs / crashes / heartbeat) ──────────────────────────
/** First-boot registration: POST {backend}/api/device/register with the per-user
 *  X-Api-Key + X-Device-Id; returns a per-device bearer token stored in NVS. */
#ifndef DEVICE_REGISTER_PATH
#define DEVICE_REGISTER_PATH "/api/device/register"
#endif
#ifndef TELEMETRY_LOGS_PATH
#define TELEMETRY_LOGS_PATH "/api/device/logs"
#endif
#ifndef TELEMETRY_CRASH_PATH
#define TELEMETRY_CRASH_PATH "/api/device/crashes"
#endif
#ifndef TELEMETRY_HEARTBEAT_PATH
#define TELEMETRY_HEARTBEAT_PATH "/api/device/heartbeat"
#endif
/** How often to POST a health heartbeat. Default: 15 seconds. */
#ifndef TELEMETRY_HEARTBEAT_INTERVAL_MS
#define TELEMETRY_HEARTBEAT_INTERVAL_MS (15u * 1000u)
#endif
/** How often net_poll drains the telemetry queue. */
#ifndef TELEMETRY_DRAIN_INTERVAL_MS
#define TELEMETRY_DRAIN_INTERVAL_MS 5000u
#endif
/** Max log events per POST batch. */
#ifndef TELEMETRY_BATCH_MAX
#define TELEMETRY_BATCH_MAX 16
#endif
/** Task watchdog timeout (ms). Must exceed the longest legitimate blocking op. */
#ifndef TWDT_TIMEOUT_MS
#define TWDT_TIMEOUT_MS 15000u
#endif
#ifndef AUTOMATION_POLL_INTERVAL_MS
#define AUTOMATION_POLL_INTERVAL_MS 60000u
#endif

// ── Device SD file sync ("sync to web") ────────────────────────────────────
// Polled command queue: the device drains file ops (list/manifest/push/pull/
// delete/mkdir) the web enqueued. All auth by the per-device bearer token.
// The `/upload_queue` folder is EXCLUDED on-device so this never races the
// timeline upload worker.
#ifndef DEVICE_FS_OPS_PATH
#define DEVICE_FS_OPS_PATH "/api/device/fs/ops"
#endif
#ifndef DEVICE_FS_MANIFEST_PATH
#define DEVICE_FS_MANIFEST_PATH "/api/device/fs/manifest"
#endif
#ifndef DEVICE_FS_UPLOAD_PATH
#define DEVICE_FS_UPLOAD_PATH "/api/device/fs/upload"
#endif
#ifndef DEVICE_FS_DOWNLOAD_PATH
#define DEVICE_FS_DOWNLOAD_PATH "/api/device/fs/download"
#endif
/** POST result: DEVICE_FS_OPS_PATH + "/<id>/result". */
#ifndef DEVICE_FS_POLL_INTERVAL_MS
#define DEVICE_FS_POLL_INTERVAL_MS 10000u
#endif
/** Cap on files reported in one manifest walk (bounds RAM + walk time). */
#ifndef DEVICE_FS_MANIFEST_MAX
#define DEVICE_FS_MANIFEST_MAX 400
#endif
/** Inbound LAN push: POST http://<device-ip>:AUTOMATION_HTTP_PORT/automation */
#ifndef AUTOMATION_HTTP_PORT
#define AUTOMATION_HTTP_PORT 8080
#endif
#ifndef AUTOMATION_HTTP_URI
#define AUTOMATION_HTTP_URI "/automation"
#endif
#ifndef AUTOMATION_SNOOZE_SEC
#define AUTOMATION_SNOOZE_SEC 60u
#endif
/** Skip battery light sleep if next automation fires within this window (ms). */
#ifndef AUTOMATION_SKIP_SLEEP_BEFORE_MS
#define AUTOMATION_SKIP_SLEEP_BEFORE_MS 120000u
#endif
#ifndef AUTOMATION_QUEUE_MAX
#define AUTOMATION_QUEUE_MAX 12
#endif


/** Light sleep: min timer duration (ms); avoids zero-length sleep when an event is already due. */
#ifndef BATTERY_SLEEP_MIN_TIMER_MS
#define BATTERY_SLEEP_MIN_TIMER_MS 50u
#endif
/** Light sleep: max timer duration (s) when next automation is far (bounds drift / sanity). */
#ifndef BATTERY_SLEEP_MAX_TIMER_SEC
#define BATTERY_SLEEP_MAX_TIMER_SEC (24u * 3600u)
#endif
/** Light sleep: timer when no pending automation (wall clock unsynced or empty queue). Still wakes on GPIO. */
#ifndef BATTERY_SLEEP_IDLE_POLL_MS
#define BATTERY_SLEEP_IDLE_POLL_MS 500u
#endif
/**
 * If > 0: cap each timer sleep to this many seconds so external power / STATS can be re-checked
 * periodically. 0 = timer follows next automation only (fastest deep sleep; USB plug may be late).
 */
#ifndef BATTERY_SLEEP_USB_POLL_MAX_SEC
#define BATTERY_SLEEP_USB_POLL_MAX_SEC 0u
#endif

/**
 * Hold A (A down, B up) — persistent shell navigation:
 * - Menu → Kami home: chord A+B (short hold) or idle timeout
 * - Kami home: A → voice-agent PTT (press=start / release=send); B → Camera; Audio Recorder via app menu
 * - Any kApp / nested app → Menu: HOLD_A_APP_EXIT_MS
 * - Camera / Voice / Music exit: HOLD_A_CAMERA_EXIT_MS / HOLD_A_VOICE_MP3_MS
 * Kami home: rotary → Menu.
 */
#ifndef HOLD_A_SHELL_NAV_MS
#define HOLD_A_SHELL_NAV_MS 450u
#endif
#ifndef HOLD_A_APP_EXIT_MS
#define HOLD_A_APP_EXIT_MS 280u
#endif
#ifndef HOLD_A_CAMERA_EXIT_MS
#define HOLD_A_CAMERA_EXIT_MS 280u
#endif
#ifndef HOLD_A_VOICE_MP3_MS
#define HOLD_A_VOICE_MP3_MS 280u
#endif

/**
 * Main app list: launch highlighted app on A release only if A was held at least this long (not edge).
 * Filters bounce; pair with MENU_TAP_A_ENTER_MAX_MS.
 */
#ifndef MENU_TAP_A_ENTER_MIN_MS
#define MENU_TAP_A_ENTER_MIN_MS 120u
#endif
#ifndef MENU_TAP_A_ENTER_MAX_MS
#define MENU_TAP_A_ENTER_MAX_MS 2500u
#endif

/** Main app list: ignore rotary detents closer than this (ms) to reduce bounce / RF noise double-steps. */
#ifndef MENU_ROTARY_DEBOUNCE_MS
#define MENU_ROTARY_DEBOUNCE_MS 90u
#endif

/** Both buttons held: fire global A+B chord if stagger exceeded edge-detection window (ms).
 *  Must stay strictly greater than the HAL edge window (kWindowMs in hal_io.cpp, 300 ms)
 *  so the press-edge path always has first crack at firing the chord. */
#ifndef MENU_CHORD_HOLD_MS
#define MENU_CHORD_HOLD_MS 320u
#endif

/** BLE device advertised name. */
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "Origami"
#endif

#include "config/ota_host.h"

/** OTA distribution origin. Public HTTPS; no device download credential. */
#ifndef OTA_BASE_URL
#define OTA_BASE_URL ""
#endif

#ifndef OTA_FIRMWARE_VERSION
#define OTA_FIRMWARE_VERSION "v0.0.0"
#endif

/** Official release builds are produced only by the controlled CI environment.
 *  Local builds remain identifiable as custom; all builds require OTA consent. */
#ifndef OTA_BUILD_OFFICIAL
#define OTA_BUILD_OFFICIAL 0
#endif

#ifndef OTA_HARDWARE_ID
#define OTA_HARDWARE_ID "n16r8-rev4"
#endif

#ifndef OTA_LAYOUT_ID
#define OTA_LAYOUT_ID "n16r8-ota-v1"
#endif

#ifndef OTA_FIRMWARE_ASSET
#define OTA_FIRMWARE_ASSET "firmware-n16r8-ota-v1.bin"
#endif
/** How often (ms) to auto-check the CDN manifest for a newer firmware. Default: 6 hours. */
#ifndef OTA_AUTO_CHECK_INTERVAL_MS
#define OTA_AUTO_CHECK_INTERVAL_MS (6u * 60u * 60u * 1000u)
#endif

/** Brief MCP read glitches while SD/SPI or I²C is busy: allow one button to read "released" briefly without resetting the hold-chord timer. */
#ifndef MENU_CHORD_HOLD_GRACE_MS
#define MENU_CHORD_HOLD_GRACE_MS 120u
#endif
