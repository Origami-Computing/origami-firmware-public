#include "services/vibration.h"
#include "services/automation.h"
#include "core/alert_vibration.h"
#include "services/recipe_alerts.h"
#include "core/store.h"

#include "Pins.h"
#include "core/board_time.h"
#include "core/menu_config.h"
#include "hal/hal_io.h"
#include "services/morning_quote.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

namespace services {
namespace automation {

void transport_poll_service(uint32_t ms);
void beginRecipeTransport();

bool wallClockSynced();
uint16_t rgb565ForType(Type);

struct QEntry {
  bool recipe;
  uint32_t server_id;
  Type     type;
  AlertSound sound;
  uint8_t vibration;
  AlertKind kind;
  time_t   fire_at;
  char     msg[128];
};

static QEntry s_q[AUTOMATION_QUEUE_MAX];
static services::recipe_alerts::Queue s_recipes;
static bool s_recipeEnabled = false;
static bool s_alert_recipe = false;
static int    s_q_count = 0;

static uint32_t s_alert_server_id=0;
static bool s_alert_active = false;
static bool s_showSnooze = false;
static time_t s_snoozeUntil = 0;
static constexpr uint32_t kSnoozeServerId = 0xffffffe0u;
static Type s_alert_type   = Type::Yellow;
static AlertSound s_alert_sound = AlertSound::Classic;
static uint8_t s_alert_vibration = 0;
static char s_alert_msg[128];
static uint32_t s_alert_started_ms = 0;
static AlertKind s_alert_kind = AlertKind::Standard;
static core::morning_alarm::Session s_morning;
static QEntry s_morning_entry;

static bool s_ota_pending = false;

static uint32_t s_effect_phase_ms = 0;
static bool     s_user_activity_pulse = false;
static bool     s_morning_routine_pending = false;

// ---------------------------------------------------------------------------
// Time / queue
// ---------------------------------------------------------------------------

static bool serverIdExists(uint32_t sid) {
  if (sid == 0) return false;
  for (int i = 0; i < s_q_count; i++) {
    if (s_q[i].server_id == sid) return true;
  }
  return false;
}

static bool insertSorted(QEntry e) {
  if (s_q_count >= (int)AUTOMATION_QUEUE_MAX) return false;
  int pos = 0;
  while (pos < s_q_count && s_q[pos].fire_at <= e.fire_at) pos++;
  for (int i = s_q_count; i > pos; i--) s_q[i] = s_q[i - 1];
  s_q[pos] = e;
  s_q_count++;
  return true;
}

static bool popFront(QEntry* out) {
  if (s_q_count <= 0 || !out) return false;
  *out = s_q[0];
  for (int i = 1; i < s_q_count; i++) s_q[i - 1] = s_q[i];
  s_q_count--;
  return true;
}

static bool earliestFire(time_t* out) {
  if (!out) return false;
  bool found=s_recipeEnabled && s_recipes.earliest(*out);
  if(s_q_count>0 && (!found || s_q[0].fire_at<*out)) { *out=s_q[0].fire_at; found=true; }
  return found;
}

static void stopAlertOutputs() {
  services::vibration::release(services::vibration::Owner::Alarm);
}

static void startAlert(const QEntry& e) {
  s_showSnooze = false;
  s_alert_active    = true;
  s_alert_recipe    = e.recipe;
  s_alert_type      = e.type;
  s_alert_sound     = e.sound;
  s_alert_vibration = e.vibration;
  s_alert_kind      = e.kind;
  s_alert_server_id = e.server_id;
  if (e.kind == AlertKind::Morning) {
    s_morning_entry = e;
    s_morning.start(millis());
    services::morning_quote::requestForEpoch(e.fire_at > 0 ? e.fire_at : ::time(nullptr));
    Serial.println("[morning] ringing");
  }
  strlcpy(s_alert_msg, e.msg, sizeof(s_alert_msg));
  s_alert_started_ms = millis();
  s_effect_phase_ms  = 0;
}

// ---------------------------------------------------------------------------
// Type helpers / colors (per-type hook — same vibrate/sound for now)
// ---------------------------------------------------------------------------

/** Short “off” pulses only (~17% off) so it feels like steady rumble — unrelated to message blink. */
static void applyDefaultVibratePattern(uint32_t phase) {
  services::vibration::drive(services::vibration::Owner::Alarm,s_alert_vibration,phase);
}

static void tickEffects(Type t, uint32_t phase_ms) {
  (void)t;
  applyDefaultVibratePattern(phase_ms);
}

static void snoozeCurrent() {
  if (!s_alert_active) return;
  if (s_alert_kind == AlertKind::Morning) {
    stopAlertOutputs();
    s_morning.snooze(millis());
    s_alert_active = false;
    s_user_activity_pulse = true;
    Serial.println("[morning] sleeping 300 seconds");
    return;
  }
  stopAlertOutputs();
  if (!wallClockSynced()) {
    s_alert_active = false;
    s_user_activity_pulse = true;
    return;
  }
  time_t now = ::time(nullptr);
  QEntry e{};
  e.recipe    = s_alert_recipe;
  e.server_id = kSnoozeServerId;
  e.type      = s_alert_type;
  e.sound     = s_alert_sound;
  e.vibration = s_alert_vibration;
  e.fire_at   = now + (time_t)AUTOMATION_SNOOZE_SEC;
  strlcpy(e.msg, s_alert_msg, sizeof(e.msg));
  cancelPendingWithServerId(kSnoozeServerId);
  if (!insertSorted(e)) return;
  s_snoozeUntil = e.fire_at;
  s_showSnooze = true;
  s_alert_active      = false;
  s_user_activity_pulse = true;
}

static void dismissAlert() {
  if (s_showSnooze) {
    cancelPendingWithServerId(kSnoozeServerId);
    s_showSnooze = false;
    s_snoozeUntil = 0;
    s_user_activity_pulse = true;
    return;
  }
  if (s_morning.phase == core::morning_alarm::Phase::Greeting && !s_alert_active) {
    s_morning.phase = core::morning_alarm::Phase::Idle;
    s_morning_routine_pending = true;
    s_user_activity_pulse = true;
    return;
  }
  if (morningAlarmPhase() == core::morning_alarm::Phase::Sleeping ||
      (s_alert_active && s_alert_kind == AlertKind::Morning)) {
    stopAlertOutputs();
    s_morning.acknowledge(millis());
    s_alert_active = false;
    s_user_activity_pulse = true;
    Serial.println("[morning] good morning");
    return;
  }
  if (!s_alert_active) return;
  stopAlertOutputs();
  s_alert_active        = false;
  s_user_activity_pulse = true;
}

static void fireDueEvents() {
  if (s_alert_active) return;
  if (!wallClockSynced()) return;
  time_t now = ::time(nullptr);
  while (s_q_count > 0 && s_q[0].fire_at <= now) {
    QEntry e;
    if (!popFront(&e)) break;
    if(e.recipe && (!s_recipeEnabled || e.fire_at < now-services::recipe_alerts::kMaxLateness)) continue;
    startAlert(e);
    break;
  }
  if (!s_alert_active && s_recipeEnabled) {
    services::recipe_alerts::Entry reminder;
    if (s_recipes.takeDue(now,reminder)) {
      // Persist before ringing, so a reboot or replay cannot ring it twice.
      s_recipes.remember(reminder.id);
      if(!core::store::putBlob("recipe_alerts","receipts",&s_recipes.receipts,sizeof(s_recipes.receipts))) {
        Serial.println("[recipe-alerts] receipt save failed; skipped alarm");
        return;
      }
      QEntry e{}; e.recipe=true; e.type=reminder.type; e.fire_at=reminder.fire;
      e.sound=AlertSound::Classic;
      strlcpy(e.msg,reminder.message,sizeof(e.msg));
      startAlert(e);
    }
  }
}

bool wallClockSynced() {
  // Single source of truth: the clock is only trustworthy once SNTP has synced it
  // this session (core::time::init() zeroes it at boot so a stale RTC value can't
  // arm alarms). Keeps the display gate and the alarm gate in perfect agreement.
  return core::time::isSynced();
}

uint16_t rgb565ForType(Type t) {
  switch (t) {
    case Type::Yellow: return 0xFFE0;
    case Type::Red: return 0xF800;
    case Type::Green: return 0x07E0;
  }
  return 0xFFE0;
}

bool ingestScheduledWithSound(Type t, time_t fire_unix_utc, const char* message,
                              uint32_t server_id, AlertSound sound, AlertKind kind, uint8_t vibration) {
  if (!message) return false;
  if (serverIdExists(server_id)) return true;

  QEntry e{};
  e.server_id = server_id;
  e.type      = t;
  e.sound     = sound;
  e.vibration = core::alert_vibration::validId(vibration);
  e.kind      = kind;
  e.fire_at   = fire_unix_utc;
  strlcpy(e.msg, message, sizeof(e.msg));

  time_t now = ::time(nullptr);
  if (wallClockSynced() && e.fire_at <= now) {
    if (!insertSorted(e)) return false;
    fireDueEvents();
    return true;
  }
  return insertSorted(e);
}

bool ingestScheduled(Type t, time_t fire_unix_utc, const char* message, uint32_t server_id) {
  return ingestScheduledWithSound(t, fire_unix_utc, message, server_id, AlertSound::Classic);
}

bool schedule(Type t, time_t fire_unix_utc, const char* message) {
  return ingestScheduled(t, fire_unix_utc, message, 0);
}

void cancelAllPending() {
  s_q_count = 0;
}

int pendingAutomationsCount() {
  return s_q_count;
}

bool peekPendingAutomationDetail(int index, Type* type, time_t* fire_unix_utc, char* msg_out, size_t msg_cap,
                                 uint32_t* server_id_out) {
  if (index < 0 || index >= s_q_count || !type || !fire_unix_utc || !msg_out || msg_cap == 0) return false;
  const QEntry& e = s_q[index];
  *type           = e.type;
  *fire_unix_utc  = e.fire_at;
  strlcpy(msg_out, e.msg, msg_cap);
  if (server_id_out) *server_id_out = e.server_id;
  return true;
}

bool peekPendingAutomation(int index, Type* type, time_t* fire_unix_utc, char* msg_out, size_t msg_cap) {
  return peekPendingAutomationDetail(index, type, fire_unix_utc, msg_out, msg_cap, nullptr);
}

bool hasPendingAutomationWithServerId(uint32_t server_id) {
  if (server_id == 0) return false;
  for (int i = 0; i < s_q_count; i++) {
    if (s_q[i].server_id == server_id) return true;
  }
  return false;
}

bool cancelPendingAutomationAtIndex(int index) {
  if (index < 0 || index >= s_q_count) return false;
  for (int i = index; i < s_q_count - 1; i++) s_q[i] = s_q[i + 1];
  s_q_count--;
  return true;
}

bool cancelPendingWithServerId(uint32_t server_id) {
  if (server_id == 0) return false;
  bool removed = false;
  int w = 0;
  for (int r = 0; r < s_q_count; r++) {
    if (s_q[r].server_id == server_id) {
      removed = true;
      continue;
    }
    if (w != r) s_q[w] = s_q[r];
    w++;
  }
  s_q_count = w;
  return removed;
}

bool isAlertActive()       { return s_alert_active; }
uint32_t activeAlertServerId() { return s_alert_server_id; }
Type activeAlertType()     { return s_alert_type; }
const char* activeAlertMsg() { return s_alert_msg; }
AlertSound activeAlertSound() { return s_alert_sound; }
core::morning_alarm::Phase morningAlarmPhase() {
  if (s_alert_active && s_alert_kind != AlertKind::Morning) return core::morning_alarm::Phase::Idle;
  return s_morning.phase;
}
uint32_t morningAlarmPhaseStartedMs() { return s_morning.startedMs; }
uint32_t morningAlarmSnoozeRemainingMs() { return s_morning.remainingMs(millis()); }
bool previewMorningAlarm(AlertSound sound, uint8_t vibration) {
  if (wantsInputCapture()) return false;
  QEntry entry{};
  entry.type = Type::Red;
  entry.kind = AlertKind::Morning;
  entry.sound = sound;
  entry.vibration = core::alert_vibration::validId(vibration);
  strlcpy(entry.msg, "Morning Alarm", sizeof(entry.msg));
  startAlert(entry);
  return true;
}

bool takeMorningRoutineRequest() {
  if (!s_morning_routine_pending) return false;
  s_morning_routine_pending = false;
  return true;
}

void snoozeActiveAlert() { snoozeCurrent(); }
void dismissActiveAlert() { dismissAlert(); }

bool activeAlertUsesRedStatusBar() {
  return s_alert_active && s_alert_type == Type::Red && s_alert_kind != AlertKind::Morning;
}

bool showingSnooze() { return s_showSnooze; }
uint32_t snoozeRemainingSeconds() {
  const time_t now = ::time(nullptr);
  return s_snoozeUntil > now ? (uint32_t)(s_snoozeUntil-now) : 0;
}
void leaveSnoozeScreen() { s_showSnooze=false; s_user_activity_pulse=true; }
bool wantsInputCapture() { return s_alert_active || s_showSnooze || s_morning.phase != core::morning_alarm::Phase::Idle; }

void begin() {
  core::store::getBlob("recipe_alerts","enabled",&s_recipeEnabled,sizeof(s_recipeEnabled));
  core::store::getBlob("recipe_alerts","receipts",&s_recipes.receipts,sizeof(s_recipes.receipts));
  for(auto& id:s_recipes.receipts.ids) id[sizeof(id)-1]=0;
  s_recipes.receipts.cursor %= services::recipe_alerts::kReceiptCount;
  beginRecipeTransport();
}

bool recipeAlertsEnabled() { return s_recipeEnabled; }
bool activeAlertIsRecipe() { return s_alert_active && s_alert_recipe; }
unsigned pendingRecipeAlertsCount() { return s_recipes.pending.count; }
bool setRecipeAlertsEnabled(bool enabled) {
  if (!core::store::putBlob("recipe_alerts","enabled",&enabled,sizeof(enabled))) return false;
  s_recipeEnabled=enabled;
  if(!enabled) {
    s_recipes.clear();
    for(int i=s_q_count-1;i>=0;--i) if(s_q[i].recipe) cancelPendingAutomationAtIndex(i);
    if(s_alert_recipe) {
      if(s_alert_active) { stopAlertOutputs(); s_alert_active=false; }
      s_showSnooze=false; s_snoozeUntil=0; s_user_activity_pulse=true;
    }
  }
  return true;
}
void applyRecipeSnapshot(const services::recipe_alerts::Snapshot& snapshot) {
  if(s_recipeEnabled && snapshot.count<=services::recipe_alerts::kCapacity) s_recipes.replace(snapshot);
}
bool ingestRecipeAlert(const services::recipe_alerts::Entry& entry) {
  return !s_recipeEnabled || s_recipes.upsert(entry);
}

void notifyWifiConnected(bool connected) {
  (void)connected;
}

bool takeUserActivityPulse() {
  if (!s_user_activity_pulse) return false;
  s_user_activity_pulse = false;
  return true;
}

void service(uint32_t millis_now) {
  transport_poll_service(millis_now);

  const auto previousPhase = s_morning.phase;
  s_morning.tick(millis_now, s_alert_active);
  if (previousPhase == core::morning_alarm::Phase::Greeting &&
      s_morning.phase == core::morning_alarm::Phase::Idle) {
    s_morning_routine_pending = true;
    Serial.println("[morning] routine requested");
  }
  if (previousPhase == core::morning_alarm::Phase::Sleeping &&
      s_morning.phase == core::morning_alarm::Phase::Ringing) startAlert(s_morning_entry);

  if (wallClockSynced()) fireDueEvents();

  if (s_alert_active) {
    s_effect_phase_ms = millis_now - s_alert_started_ms;
    tickEffects(s_alert_type, s_effect_phase_ms);
    return;
  }

  // Snooze/dismiss explicitly stop our outputs. Do not overwrite another
  // foreground app's vibration on every idle automation tick.
}

bool shouldDeferBatterySleep() {
  if (!wallClockSynced()) return false;
  time_t nf = 0;
  if (!earliestFire(&nf)) return false;
  time_t now = ::time(nullptr);
  if ((double)(nf - now) > 0 && (double)(nf - now) * 1000.0 <= (double)AUTOMATION_SKIP_SLEEP_BEFORE_MS) return true;
  return false;
}

void setOtaPending()        { s_ota_pending = true; }
bool otaUpdateRequested()   { return s_ota_pending; }
void clearOtaRequest()      { s_ota_pending = false; }

bool shouldExitBatterySleepLoop() {
  if (!wallClockSynced()) return false;
  time_t now = ::time(nullptr);
  time_t nf  = 0;
  if (!earliestFire(&nf)) return false;
  return nf <= now;
}

}  // namespace automation
}  // namespace services
