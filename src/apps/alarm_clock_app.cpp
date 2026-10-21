#include "sfx/alarm_catalog.h"
#include "services/vibration.h"
#include "config/screen_chrome_layout.h"
/**
 * @file alarm_clock_app.cpp
 * @brief Multi-alarm clock with list-first navigation and local persistence.
 */

#include "apps/alarm_clock_app.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/automation.h"
#include "core/alert_vibration.h"
#include "core/board_time.h"
#include "services/alarms.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "scene/screen.h"
#include "sfx/sfx_lib.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/source_layout.h"

namespace apps {
namespace alarm_clock {

extern const ::scene::Screen kScreenAlarmList;
extern const ::scene::Screen kScreenAlarmEdit;

namespace {

using services::alarms::Alarm;
using services::alarms::kMaxAlarms;
static constexpr uint16_t kEnabledDotColor = 0x07E0;

enum class ScreenMode : uint8_t { List, Edit };
enum class EditField : uint8_t { Hours, Minutes, Sound, Vibration, Enabled, Save };

static bool s_active = false;
static bool s_needRedraw = false;
static ScreenMode s_mode = ScreenMode::List;
static EditField s_editField = EditField::Hours;
static bool s_adjustingField = false;
static bool s_editHighlight = true;
static uint32_t s_editBlinkMs = 0;
static constexpr uint32_t kEditBlinkIntervalMs = 500;

static const auto& s_catalog = services::alarms::snapshot();
static ui::ListView s_alarmList;

static Alarm s_editAlarm;
static bool s_editingMorning = false;
static const char* s_saveError = nullptr;
static bool s_lastMorningScheduled = false;
static uint8_t s_editIndex = 0;

static int alarmListRowCount() {
  return (int)s_catalog.count + 2;  // Morning Alarm, existing alarms, Add Alarm
}

static void seedNewAlarmFromClock() {
  s_editAlarm = Alarm{};
  if (!services::automation::wallClockSynced()) return;

  const time_t now = time(nullptr);
  struct tm localTime;
  localtime_r(&now, &localTime);
  s_editAlarm.hour = (uint8_t)localTime.tm_hour;
  s_editAlarm.minute = (uint8_t)localTime.tm_min;
}

static void wrapHour(int delta) {
  int hour = (int)s_editAlarm.hour + delta;
  while (hour < 0) hour += 24;
  while (hour > 23) hour -= 24;
  s_editAlarm.hour = (uint8_t)hour;
}

static void wrapMinute(int delta) {
  int minute = (int)s_editAlarm.minute + delta;
  while (minute < 0) minute += 60;
  while (minute > 59) minute -= 60;
  s_editAlarm.minute = (uint8_t)minute;
}

static void changeSound(int delta) {
  int value = (int)s_editAlarm.sound + delta;
  constexpr int kSoundCount = 4;
  while (value < 0) value += kSoundCount;
  while (value >= kSoundCount) value -= kSoundCount;
  s_editAlarm.sound = (services::automation::AlertSound)value;
}

static void markRedraw() {
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static bool fieldHighlighted(EditField field) {
  return s_editField == field && (!s_adjustingField || s_editHighlight);
}

static void openAlarm(uint8_t index) {
  if (index >= s_catalog.count) return;
  s_editingMorning = false;
  s_saveError = nullptr;
  s_editIndex = index;
  s_editAlarm = s_catalog.alarms[index];
  s_editField = EditField::Hours;
  sfx::play(sfx::kSfxAlarmOpen);
  ::scene::switchTo(&app, &kScreenAlarmEdit);
}

static void beginNewAlarm() {
  if (s_catalog.count >= kMaxAlarms) {
    sfx::play(sfx::kSfxAlarmError);
    return;
  }
  s_editIndex = s_catalog.count;
  s_editingMorning = false;
  s_saveError = nullptr;
  seedNewAlarmFromClock();
  s_editField = EditField::Hours;
  sfx::play(sfx::kSfxAlarmAdd);
  ::scene::switchTo(&app, &kScreenAlarmEdit);
}

static void saveEditedAlarm() {
  if (s_editingMorning) {
    if (!services::alarms::saveMorning(s_editAlarm)) {
      s_saveError = "Couldn't save. Try again.";
      sfx::play(sfx::kSfxAlarmError);
      markRedraw();
      return;
    }
    s_alarmList.selected = 0;
    sfx::play(sfx::kSfxAlarmSave);
    ::scene::switchTo(&app, &kScreenAlarmList, ::scene::ScreenTransition::Back);
    return;
  }
  if (!services::alarms::save(s_editIndex, s_editAlarm)) return;
  s_alarmList.totalItems = alarmListRowCount();
  s_alarmList.selected = s_editIndex + 1;
  s_alarmList.clampScroll();
  sfx::play(sfx::kSfxAlarmSave);
  ::scene::switchTo(&app, &kScreenAlarmList, ::scene::ScreenTransition::Back);
}

static void cancelEdit() {
  s_alarmList.totalItems = alarmListRowCount();
  s_alarmList.clampScroll();
  ::scene::switchTo(&app, &kScreenAlarmList, ::scene::ScreenTransition::Back);
}

static ui::ListItem alarmItemFn(int index, void*) {
  static char label[16];
  ui::ListItem item;
  if (index < 0 || index >= alarmListRowCount()) return item;
  if (index == 0) {
    item.label = "Morning Alarm";
    item.fg = kUiFg;
    item.choice = true;
    item.checked = s_catalog.morning.enabled;
    item.indicatorFg = kEnabledDotColor;
    return item;
  }
  index--;
  if (index == s_catalog.count) {
    item.label = "Add Alarm";
    item.fg = kUiFg;
    return item;
  }

  const Alarm& alarm = s_catalog.alarms[index];
  snprintf(label, sizeof(label), "%02u:%02u",
           (unsigned)alarm.hour, (unsigned)alarm.minute);
  item.label = label;
  item.fg = alarm.enabled ? kUiFg : kUiMuted;
  item.choice = true;
  item.checked = alarm.enabled;
  item.indicatorFg = kEnabledDotColor;
  return item;
}

static void buildAlarmListScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder builder(scene, width, height);
  builder.clearContent(kUiBg);

  s_alarmList.totalItems = alarmListRowCount();
  s_alarmList.buildScene(scene, 0, kSystemStatusBarH, width,
                         height - kSystemStatusBarH - 30,
                         alarmItemFn, nullptr,
                         kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);

  char morningStatus[48];
  snprintf(morningStatus, sizeof(morningStatus), "Morning %02u:%02u - %s",
           s_catalog.morning.hour, s_catalog.morning.minute,
           !s_catalog.morning.enabled ? "Off" :
           !services::automation::wallClockSynced() || !core::time::timezoneConfigured()
               ? "Needs Wi-Fi time" : !s_catalog.morningScheduled ? "Waiting to schedule" : "Every day");
  // Scheduling failures remain visible in the page, without a control footer.
  if (s_catalog.morning.enabled && !s_catalog.morningScheduled)
    scene.addLabel("alarm.schedule",{14,(int16_t)(height-25),(int16_t)(width-28),18},"Morning alarm needs Wi-Fi time",kUiMuted,kUiBg,1,ui::scene::TextAlign::MiddleCenter,2);
  ui::scene::applySourceLayout(scene, "alarms", "list");
}

static void buildAlarmEditScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder builder(scene, width, height);
  builder.clearContent(kUiBg);
  builder.clockDisplay("alarm.clock", s_editAlarm.hour, s_editAlarm.minute,
                       s_editField == EditField::Hours,
                       s_editField == EditField::Minutes);
  builder.settingRow("alarm.sound", "Sound", sfx::alarms::preset((uint8_t)s_editAlarm.sound).name,
                     fieldHighlighted(EditField::Sound));
  builder.settingRow("alarm.vibration", "Vibration", core::alert_vibration::name(s_editAlarm.vibration),
                     fieldHighlighted(EditField::Vibration), ui::scene::Builder::RowStyle::Stacked);
  builder.onOffRow("alarm.enabled", "Enabled", s_editAlarm.enabled,
                   fieldHighlighted(EditField::Enabled));
  // Keep both blink phases black/white, including a disabled alarm.
  if (auto* label = scene.find("alarm.enabled.label")) {
    label->fgColor = fieldHighlighted(EditField::Enabled) ? kUiSelFg : kUiFg;
  }
  builder.settingRow("alarm.save", s_saveError ? "Save failed - retry" : "Save", nullptr,
                     fieldHighlighted(EditField::Save));
  ui::scene::applySourceLayout(scene, "alarms", "edit");
  if (s_adjustingField) {
    const char* id = s_editField == EditField::Sound ? "alarm.sound.value" :
                     s_editField == EditField::Vibration ? "alarm.vibration.value" :
                     s_editField == EditField::Enabled ? "alarm.enabled.label" : nullptr;
    if (id) if (auto* label=scene.find(id)) label->fgColor=kUiEdit;
  }
  // Follow the existing clock layout and font, adding a solid selection
  // surface to each digit pair without changing other apps' clock displays.
  const char* clockIds[] = {"alarm.clock.hour", "alarm.clock.minute"};
  const char* backgroundIds[] = {"alarm.clock.hour.bg", "alarm.clock.minute.bg"};
  for (uint8_t i = 0; i < 2; ++i) {
    auto* label = scene.find(clockIds[i]);
    if (!label) continue;
    const bool highlighted = fieldHighlighted(i == 0 ? EditField::Hours : EditField::Minutes);
    const bool editing = s_adjustingField && s_editField == (i == 0 ? EditField::Hours : EditField::Minutes);
    label->fgColor = editing ? kUiEdit : highlighted ? kUiSelFg : kUiFg;
    label->bgColor = highlighted ? kUiSelBg : kUiBg;
    label->z = 2;
    const auto& r = label->rect;
    scene.addRect(backgroundIds[i], {(int16_t)(r.x - 2), (int16_t)(r.y - 2),
                  (int16_t)(r.w + 4), (int16_t)(r.h + 4)}, label->bgColor, 3, 1);
  }
}

static void drawScene(TFT_eSprite& canvas,
                      void (*build)(ui::scene::Scene&, int16_t, int16_t)) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  build(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
}

}  // namespace

void enter() {
  // List initialization stops previews before storage; editor re-entry keeps its draft.
  if (s_active || app.currentScreen == &kScreenAlarmEdit) return;
  services::vibration::stopPreview();
  s_adjustingField = false;
  s_active = true;
  s_mode = ScreenMode::List;
  s_needRedraw = true;
  s_alarmList.selected = 0;
  s_alarmList.scrollFirst = 0;
  services::alarms::reload();
  s_alarmList.totalItems = alarmListRowCount();
}

void exitApp() {
  services::vibration::stopPreview();
  s_active = false;
}

void tick() {
  if (s_lastMorningScheduled != s_catalog.morningScheduled) {
    s_lastMorningScheduled = s_catalog.morningScheduled;
    if (s_active) markRedraw();
  }
  if (!s_active || s_mode != ScreenMode::Edit || !s_adjustingField) return;
  const uint32_t now = millis();
  const uint32_t intervals = (uint32_t)(now - s_editBlinkMs) / kEditBlinkIntervalMs;
  if (!intervals) return;
  s_editBlinkMs += intervals * kEditBlinkIntervalMs;
  if (intervals & 1u) {
    s_editHighlight = !s_editHighlight;
    markRedraw();
  }
}

void onInput(hal::input::InputEvent event) {
  using EventType = hal::input::EventType;
  if (!s_active) return;

  if (s_mode == ScreenMode::List) {
    if (event.type == EventType::Scroll && event.delta != 0) {
      const int previous = s_alarmList.selected;
      s_alarmList.totalItems = alarmListRowCount();
      s_alarmList.scroll(event.delta);
      if (s_alarmList.selected != previous) {
        sfx::play(sfx::kSfxAlarmNavigate);
        markRedraw();
      }
      return;
    }
    if (ui::nav::isEnterTap(event)) {
      if (s_alarmList.selected == 0) {
        s_editingMorning = true;
        s_saveError = nullptr;
        s_editAlarm = s_catalog.morning;
        s_editField = EditField::Hours;
        sfx::play(sfx::kSfxAlarmOpen);
        ::scene::switchTo(&app, &kScreenAlarmEdit);
      } else if (s_alarmList.selected <= s_catalog.count) {
        openAlarm((uint8_t)(s_alarmList.selected - 1));
      } else {
        beginNewAlarm();
      }
      return;
    }
    return;
  }

  if(event.type==EventType::ButtonB_Tap && s_editField==EditField::Vibration) {
    if(services::vibration::previewing())services::vibration::stopPreview();
    else services::vibration::preview(s_editAlarm.vibration);
    return;
  }
  if (s_editingMorning && event.type == EventType::ButtonB_Tap) {
    services::automation::previewMorningAlarm(s_editAlarm.sound,s_editAlarm.vibration);
    return;
  }

  if (event.type == EventType::Scroll && event.delta != 0) {
    if (!s_adjustingField) {
      constexpr int kFieldCount = (int)EditField::Save + 1;
      const int next = ((int)s_editField + event.delta % kFieldCount + kFieldCount) % kFieldCount;
      s_editField = (EditField)next;
      sfx::play(sfx::kSfxAlarmNavigate);
      markRedraw();
      return;
    }
    if (s_editField == EditField::Hours) wrapHour(event.delta);
    else if (s_editField == EditField::Minutes) wrapMinute(event.delta);
    else if (s_editField == EditField::Sound) {
      changeSound(event.delta);
      sfx::alarms::preview((uint8_t)s_editAlarm.sound);
    } else if (s_editField == EditField::Vibration) {
      s_editAlarm.vibration=(s_editAlarm.vibration+event.delta%core::alert_vibration::kCount+core::alert_vibration::kCount)%core::alert_vibration::kCount;
      services::vibration::preview(s_editAlarm.vibration);
    } else if (s_editField == EditField::Enabled) {
      if (abs(event.delta)%2) s_editAlarm.enabled = !s_editAlarm.enabled;
      sfx::play(s_editAlarm.enabled ? sfx::kSfxAlarmOn : sfx::kSfxAlarmOff);
    } else {
      return;
    }
    if (s_editField != EditField::Sound) sfx::play(sfx::kSfxAlarmNavigate);
    markRedraw();
    return;
  }

  if (ui::nav::isEnterTap(event)) {
    if (s_editField == EditField::Save) {
      saveEditedAlarm();
      return;
    }
    s_adjustingField = !s_adjustingField;
    s_editHighlight = true;
    s_editBlinkMs = millis();
    if (s_adjustingField && s_editField == EditField::Sound) sfx::alarms::preview((uint8_t)s_editAlarm.sound);
    else if(s_adjustingField && s_editField==EditField::Vibration)services::vibration::preview(s_editAlarm.vibration);
    else sfx::play(sfx::kSfxAlarmNavigate);
    markRedraw();
    return;
  }

  if (ui::nav::isBackTap(event)) {
    if(s_adjustingField) { s_adjustingField=false; markRedraw(); return; }
    cancelEdit();
  }
}

void onDraw(TFT_eSprite& canvas) {
  if (!s_active || !s_needRedraw) return;
  s_needRedraw = false;
  drawScene(canvas, s_mode == ScreenMode::List ? buildAlarmListScene : buildAlarmEditScene);
}

static void screenTick(uint32_t) {
  tick();
}

static void enterAlarmList() {
  services::vibration::stopPreview();
  s_adjustingField = false;
  s_mode = ScreenMode::List;
  markRedraw();
}

static void enterAlarmEdit() {
  s_mode = ScreenMode::Edit;
  s_adjustingField = false;
  s_editHighlight = true;
  markRedraw();
}

const ::scene::Screen kScreenAlarmList = {
    "Alarm List",
    enterAlarmList,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "list",
    buildAlarmListScene,
    &screen_chrome_layout::kAlarmsListTopBarColor,
};

const ::scene::Screen kScreenAlarmEdit = {
    "Alarm",
    enterAlarmEdit,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "edit",
    buildAlarmEditScene,
};

static const ::scene::Screen* const kScreens[] = {
    &kScreenAlarmList,
    &kScreenAlarmEdit,
};

App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Alarms", "alarms", &kScreenAlarmList, enter, enter, exitApp,
                            "src/apps/alarm_clock_app.cpp", kScreens, nullptr, nullptr);

}  // namespace alarm_clock
}  // namespace apps
