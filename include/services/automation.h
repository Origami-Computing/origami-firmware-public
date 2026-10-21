#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "core/morning_alarm.h"
namespace services { namespace recipe_alerts { struct Entry; struct Snapshot; } }

namespace services {
namespace automation {

enum class Type : uint8_t {
  Yellow = 0,
  Red    = 1,
  Green  = 2,
};

enum class AlertSound : uint8_t {
  Classic = 0,
  Chime,
  Digital,
  Gentle,
};

enum class AlertKind : uint8_t { Standard, Morning };

/** Restore recipe preferences and receipts before workers start. */
void begin();

/** True once SNTP wall clock looks sane (reject bogus/unset ESP32 time). */
bool wallClockSynced();

/** Schedule an automation/alarm (same API HTTP ingestion uses). Unix epoch seconds (UTC). */
bool schedule(Type t, time_t fire_unix_utc, const char* message);

/**
 * Same as schedule but optional server-side id for deduplication (poll/push). Pass 0 for local-only.
 * Used by transport; alarm apps should call schedule().
 */
bool ingestScheduled(Type t, time_t fire_unix_utc, const char* message, uint32_t server_id);

/** Schedule an alert with a specific ringing sound. */
bool ingestScheduledWithSound(Type t, time_t fire_unix_utc, const char* message,
                              uint32_t server_id, AlertSound sound,
                              AlertKind kind = AlertKind::Standard, uint8_t vibration = 0);

/** UI-task BLE payload interpretation. Borrowed JSON is consumed synchronously;
 * preserves BLE defaults/time fallback, distinct from HTTP ingestion rules. */
void ingestBleNotification(const char* json);

/** Clear pending queue (not active alert). */
void cancelAllPending();

/** Number of pending scheduled automations (sorted by fire time). */
int pendingAutomationsCount();

/** Copy pending entry `index` in [0, count); returns false if out of range or bad pointers. */
bool peekPendingAutomation(int index, Type* type, time_t* fire_unix_utc, char* msg_out, size_t msg_cap);

/** Same as peekPendingAutomation plus optional `server_id_out` (entry id, 0 = local-only). */
bool peekPendingAutomationDetail(int index, Type* type, time_t* fire_unix_utc, char* msg_out, size_t msg_cap,
                                 uint32_t* server_id_out);

/** True if the queue already has an entry with this non-zero server_id. */
bool hasPendingAutomationWithServerId(uint32_t server_id);

/** Remove pending entry at index; later entries shift down. Returns false if out of range. */
bool cancelPendingAutomationAtIndex(int index);

/** Remove one pending entry by server_id (0 = no-op). For local alarm slots / dedupe updates. */
bool cancelPendingWithServerId(uint32_t server_id);

bool isAlertActive();

/** True while the alert screen captures input, including morning sleep/greeting. */
bool wantsInputCapture();

/** Alert type of the currently active alert. Only valid when isAlertActive() is true. */
Type activeAlertType();

/** Identity of the last activated alert; interpret only while isAlertActive(). */
uint32_t activeAlertServerId();
/** Alert message of the currently active alert. Pointer into a static buffer — only valid when isAlertActive() is true. */
const char* activeAlertMsg();
AlertSound activeAlertSound();
core::morning_alarm::Phase morningAlarmPhase();
uint32_t morningAlarmPhaseStartedMs();
uint32_t morningAlarmSnoozeRemainingMs();
/** Test the wake-up flow immediately without changing the saved daily alarm. */
bool previewMorningAlarm(AlertSound sound, uint8_t vibration = 0);

/** Consume the one-shot handoff raised after the Good Morning animation. */
bool takeMorningRoutineRequest();

/** User actions owned by the active system alert Screen. */
void snoozeActiveAlert();
void dismissActiveAlert();
bool showingSnooze();
uint32_t snoozeRemainingSeconds();
void leaveSnoozeScreen();

/**
 * Main loop: transport poll, due-event firing, and alert vibration.
 * The system alert Screen owns visuals, sound, and snooze/dismiss input.
 */
void service(uint32_t millis_now);

/** Red automation alert: paint status strip red with light foreground (layout unchanged). */
bool activeAlertUsesRedStatusBar();

/** Stop LAN automation listener before WiFi is powered down (battery sleep). */
void prepareForBatterySleep();

/**
 * Run one outbound automations poll (HTTPS GET + parse). Called from net_poll on
 * Core 0 — does blocking network I/O, never touches I2C/display. Parsed entries
 * are handed to ui_task via an internal queue; drainPolledInbox() applies them.
 */
void pollTransportNow();

/** Ingest any entries fetched by pollTransportNow(). Call from ui_task each tick. */
void drainPolledInbox();


bool recipeAlertsEnabled();
bool setRecipeAlertsEnabled(bool enabled);
unsigned pendingRecipeAlertsCount();
void applyRecipeSnapshot(const services::recipe_alerts::Snapshot& snapshot);
bool ingestRecipeAlert(const services::recipe_alerts::Entry& entry);
bool activeAlertIsRecipe();
bool queueRecipeSnapshotJson(const char* json);

/** Call from menu after service if global idle should advance (snooze/dismiss). */
bool takeUserActivityPulse();

/** Before entering battery sleep: stay awake if next event soon. */
bool shouldDeferBatterySleep();

/** After each light-sleep iteration: exit sleep loop to serve due automation. */
bool shouldExitBatterySleepLoop();

uint16_t rgb565ForType(Type t);

// OTA update request — set by the automation transport when an "action":"ota"
// command arrives; svc_task consumes it and posts the UI-task update prompt.
void setOtaPending();
bool otaUpdateRequested();
void clearOtaRequest();

}  // namespace automation
}  // namespace services
