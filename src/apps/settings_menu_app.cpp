#include "services/vibration.h"
#include "config/screen_chrome_layout.h"
#include "core/release_policy.h"
/**
 * @file settings_menu_app.cpp
 * @brief Hierarchical settings menu and editors.
 *
 * Navigation
 * ----------
 * LB and LB+RB return one level. At SUB_MAIN they yield to NavHandler so
 * Settings pops back to Menu. Encoder press enters categories or commits.
 * WiFi Setup and Hardware Test are pushed as separate apps via NavHandler::push() —
 * they return to Settings automatically when their NavHandler entry pops.
 *
 * Dynamic labels
 * --------------
 * Server, sleep, and Tap Hearts labels point to static buffers that are
 * refreshed whenever their setting changes.
 *
 * Upload controls use mutex-protected snapshots and stable item IDs. The page
 * refreshes at the field-blink cadence; upload scheduling stays on the service task.
 */

#include "apps/settings_menu_app.h"

#include <Arduino.h>
#include "app.h"
#include <WiFi.h>
#include <cstring>

#include "services/agent_transport.h"
#include "services/account_info.h"
#include "services/audio_output.h"
#include "apps/hardware_test_app.h"
#include "apps/imu_calibration_app.h"
#include "apps/ota_update_app.h"
#include "services/firmware_update.h"
#include "apps/tutorial_app.h"
#include "assets/menu_icons.h"
#include "services/uploads.h"
#include "services/device_fs.h"
#include "hal/hal_sd.h"
#include "apps/wifi_setup_app.h"
#include "nav_handler.h"
#include "services.h"
#include "config/app_config.h"
#include "config/settings_layout.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "core/secrets.h"
#include "core/menu_config.h"
#include "services/ota_client.h"
#include "core/store.h"
#include "services/automation.h"
#include "core/sleep_timer.h"
#include "services/wifi_provision.h"
#include "hal/hal_audio.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_wifi.h"
#include "scene/screen.h"
#include "sfx/sfx.h"
#include "sfx/sfx_lib.h"
#include "ui/home_eyes.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/edit_field.h"
#include "ui/morning_style.h"
#include "ui/source_layout.h"
#include "ui/listening_style.h"

namespace apps {
namespace settings_menu {

static bool s_needRedraw = false;
static bool s_valueEditing=false, s_valueBlink=true;
static int s_valueDraft=0;
static ui::ListView s_aboutList;
static uint32_t s_aboutRefresh=0;
static uint32_t s_valueBlinkMs=0;

enum Sub {
  SUB_MAIN,
  SUB_WIFI_MENU,
  SUB_UPLOADS,
  SUB_FILE_SYNC,
  SUB_SOUND_MENU,
  SUB_AGENT_STYLE,
  SUB_MUSIC_OUTPUT,
  SUB_SFX_VOLUME,
  SUB_WIFI_NETS,
  SUB_WIFI_NET_ACTION,
  SUB_SLEEP_MENU,
  SUB_SLEEP_TIMER,
  SUB_SENSORS_MENU,
  SUB_ABOUT,
};
static Sub s_sub    = SUB_MAIN;
static int s_sel    = 0;
static bool s_updateNotice = false;
static bool s_preserveMainSelection = false;
static Sub s_resumeSub = SUB_MAIN;

extern const ::scene::Screen kScreenSettingsMain;
extern const ::scene::Screen kScreenSettingsWifiMenu;
extern const ::scene::Screen kScreenSettingsUploads;
extern const ::scene::Screen kScreenSettingsFileSync;
extern const ::scene::Screen kScreenSettingsSoundMenu;
extern const ::scene::Screen kScreenSettingsAgentStyle;
extern const ::scene::Screen kScreenSettingsMusic;
extern const ::scene::Screen kScreenSettingsSfxVolume;
extern const ::scene::Screen kScreenSettingsWifi;
extern const ::scene::Screen kScreenSettingsNetworkAction;
extern const ::scene::Screen kScreenSettingsSleepMenu;
extern const ::scene::Screen kScreenSettingsSleepTimer;
extern const ::scene::Screen kScreenSettingsSensorsMenu;
extern const ::scene::Screen kScreenSettingsAbout;

static void showMain();
static void showWifiMenu();
static void showUploads();
static void showSoundMenu();
static void showAgentStyle();
static void showMusicOutput();
static void showSfxVolume();
static void showWifiNetworks(bool resetSelection);
static void showNetworkAction();
static void showSleepMenu();
static void showSleepTimer();
static void showSensorsMenu();
static void showAbout();

// Row indices for sub-action dispatch in onInput(). Keep these in sync with
// kLabels[] below — they're the single source of truth for the row order so
// the dynamic labels (server/heart) update the right slots too.
enum MainRow {
  ROW_WIFI = 0,
  ROW_SOUND,
  ROW_RECIPE_ALERTS,
  ROW_AGENT_STYLE,
  ROW_REPLAY_TUTORIAL,
  ROW_SERVER,
  ROW_SLEEP,
  ROW_SENSORS,
  ROW_OTA_CHANNEL,
  ROW_UPDATE_FW,
  ROW_REBOOT,
  ROW_ABOUT,
  ROW_UPLOADS,
  kRows,
};

static const char* kLabels[kRows] = {
    "WiFi",
    "Sound",
    "Recipe alerts off",
    "Agent UI",
    "Redo Tutorial",
    "Server: Dev",
    "Sleep",
    "Sensors",
    "Updates: Public",
    "Update Firmware!",
    "Reboot Device!",
    "About",
    "Upload Queue",
};

static char s_serverLabel[24] = "Server: Dev";
static char s_recipeLabel[32] = "Recipe alerts off";
static void updateRecipeLabel() {
  snprintf(s_recipeLabel,sizeof(s_recipeLabel),"Recipe alerts %s",services::automation::recipeAlertsEnabled()?"on":"off");
  kLabels[ROW_RECIPE_ALERTS]=s_recipeLabel;
}
static char s_sleepLabel[20]  = "Sleep: on";
static char s_sleepTimerLabel[24] = "Timer: 60s";
static uint32_t s_sleepTimerDraft = core::sleep_timer::kDefaultSeconds;
static bool s_sleepSaveFailed = false;
static bool s_sleepEditing = false;
static int s_sleepField = 0;
static bool s_sleepBlink = true;
static uint32_t s_sleepBlinkMs = 0;
static char s_otaChannelLabel[128] = "Updates: Public";

static void updateSleepLabel() {
  snprintf(s_sleepLabel, sizeof(s_sleepLabel), "Sleep: %s",
           core::sleep_timer::enabled() ? "on" : "off");
  snprintf(s_sleepTimerLabel, sizeof(s_sleepTimerLabel), "Timer: %lus",
           (unsigned long)core::sleep_timer::seconds());
}

static void updateOtaChannelLabel() {
  snprintf(s_otaChannelLabel, sizeof(s_otaChannelLabel), "Updates: %s",
           services::ota_client::releaseChannelName(services::ota_client::releaseChannel()));
  kLabels[ROW_OTA_CHANNEL] = s_otaChannelLabel;
}

enum WifiRow { WIFI_QR, WIFI_HELP, WIFI_NETWORKS, kWifiRows };
static const char* kWifiLabels[kWifiRows] = {
    "Join WiFi (QR)", "How to Join WiFi", "Saved Networks"};

enum SoundRow { SOUND_MUSIC_OUTPUT, SOUND_SFX_VOLUME, kSoundRows };
static const char* kSoundLabels[kSoundRows] = {
    "Music Output", "SFX Volume"};

enum SleepRow { SLEEP_TOGGLE, SLEEP_TIMER, SLEEP_DEFAULT, SLEEP_NOW, kSleepRows };
static const char* kSleepLabels[kSleepRows] = {
    s_sleepLabel, s_sleepTimerLabel, "Default: 60s", "Sleep Now"};

enum SensorRow { SENSOR_HARDWARE_TEST, SENSOR_IMU_CALIBRATION, kSensorRows };
static const char* kSensorLabels[kSensorRows] = {"Hardware Test", "IMU Calibration"};

static int s_wifiNetSel    = 0;
static int s_netActionSel  = 0;
static ui::ListView s_wifiList;
static ui::ListView s_netActionList;
static constexpr int kNetActionRows = 2;
static const char*   kNetActionLabels[kNetActionRows] = {"Connect", "Delete"};
static ui::ListView s_mainList;
static ui::ListView s_wifiMenuList;
static ui::ListView s_soundMenuList;
static ui::ListView s_sleepMenuList;
static ui::ListView s_sensorsMenuList;
static ui::ListView s_musicList;
static ui::ListView s_agentStyleList;
static int s_agentStyleSel=0;
static bool s_agentStyleSaveFailed=false;
static int s_wifiMenuSel = 0;
static int s_soundMenuSel = 0;
static int s_sleepMenuSel = 0;
static int s_sensorsMenuSel = 0;

static constexpr int kMusicOutRows = 4;
static const char* kMusicOutLabels[kMusicOutRows] = {"Speaker", "Aux", "Both", "Auto"};
static int s_musicOutSel = 0;

static int musicOutRowFromRoute(hal::audio::OutputRoute r) {
  switch (r) {
    case hal::audio::OutputRoute::SpeakerOnly:    return 0;
    case hal::audio::OutputRoute::HeadphonesOnly: return 1;
    case hal::audio::OutputRoute::Auto:           return 3;
    default:                                       return 2;
  }
}

static hal::audio::OutputRoute musicOutRouteFromRow(int row) {
  switch (row) {
    case 0: return hal::audio::OutputRoute::SpeakerOnly;
    case 1: return hal::audio::OutputRoute::HeadphonesOnly;
    case 3: return hal::audio::OutputRoute::Auto;
    default: return hal::audio::OutputRoute::Both;
  }
}

static UploadQueue::View s_uploadView;
static char s_uploadId[24] = {}, s_uploadNotice[64] = {};
static int s_uploadField = 0;
static bool s_uploadEditing = false, s_uploadBlink = true;
static uint32_t s_uploadTickMs = 0, s_uploadNoticeMs = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static UploadQueue* s_uploadPreview = nullptr;
void setUploadPreviewForDevelopment(UploadQueue* queue) {
  s_uploadPreview=queue; s_uploadId[0]=0; s_uploadView.index=0;
  s_uploadNotice[0]=0; s_uploadEditing=false; s_needRedraw=true;
}
#endif
static UploadQueue& settingsUploadQueue() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(s_uploadPreview) return *s_uploadPreview;
#endif
  return services::uploads::queue();
}

static void refreshUploadView() {
  s_uploadView=settingsUploadQueue().view(s_uploadId,s_uploadView.index);
  strlcpy(s_uploadId,s_uploadView.index>=0?s_uploadView.item.id:"",sizeof(s_uploadId));
  if(!s_uploadView.count) s_uploadEditing=false;
}

static const char* s_fileSyncError = nullptr;
static uint32_t s_fileSyncTickMs = 0;
static void requestFileSync() {
  s_fileSyncError = nullptr;
  if (!hal::sd::mounted()) s_fileSyncError = "No SD card";
  else if (!core::secrets::backendConfig().apiKey[0]) s_fileSyncError = "Pair device in WiFi Setup";
  else if (!hal::wifi::connected() && !core::secrets::wifiNetworkCount())
    s_fileSyncError = "Set up WiFi first";
  else {
    if (!hal::wifi::connected()) services::wifi_provision::connectSaved(core::secrets::wifiNetworkSSID(0));
    services::device_fs::requestSync(services::device_fs::SyncRequest::Interactive);
  }
  s_needRedraw = true;
}

static void uploadNotice(const char* text) {
  strlcpy(s_uploadNotice,text,sizeof(s_uploadNotice));
  s_uploadNoticeMs=millis(); s_needRedraw=true;
}

static void updateServerLabel() {
  snprintf(s_serverLabel, sizeof(s_serverLabel), "Server: %s",
           core::secrets::isDevMode() ? "Dev" : "Prod");
  kLabels[ROW_SERVER] = s_serverLabel;
}

static void prepareWorkflow() {
  // Main entry retries initialization after a prior SD/queue failure. Returning
  // from an overlay on another Settings view must not introduce a new attempt.
  if (app.currentScreen == &kScreenSettingsMain) services::uploads::ensureInit();
}

static void enterMainView() {
  s_valueEditing = false;
  s_sub        = SUB_MAIN;
  s_updateNotice = services::firmware_update::settingsUpdateAvailable();
  if (!s_preserveMainSelection) s_sel = s_updateNotice ? -1 : 0;
  if (!s_updateNotice && s_sel < 0) s_sel = 0;
  s_preserveMainSelection = false;
  s_needRedraw = true;
  updateServerLabel();
  updateRecipeLabel();
  updateSleepLabel();
  ui::home_eyes::setTapHeartsEnabled(false);
  updateOtaChannelLabel();
  hal::display::markAllDirty();

  const Sub resume = s_resumeSub;
  s_resumeSub = SUB_MAIN;
  if (resume == SUB_WIFI_MENU) showWifiMenu();
  else if (resume == SUB_SOUND_MENU) showSoundMenu();
  else if (resume == SUB_SLEEP_MENU) showSleepMenu();
  else if (resume == SUB_SENSORS_MENU) showSensorsMenu();
}

void exitApp() {
  ui::listening_style::exitPreview();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  s_uploadPreview=nullptr;
#endif
  s_sub = SUB_MAIN;
  s_resumeSub = SUB_MAIN;
}

void tick() {
  if(s_sub==SUB_ABOUT && millis()-s_aboutRefresh>=500u){s_aboutRefresh=millis();s_needRedraw=true;}
  if (s_sub == SUB_MAIN && !s_valueEditing) {
    services::firmware_update::checkSettingsNotice();
    const bool available = services::firmware_update::settingsUpdateAvailable();
    if (available != s_updateNotice) {
      s_updateNotice = available;
      if (!available && s_sel < 0) s_sel = 0;
      s_needRedraw = true; hal::display::markAllDirty();
      Serial.printf("[ota-notice] settings row=%d\n", available);
    }
  }
  if(s_valueEditing && millis()-s_valueBlinkMs>=450u) {
    s_valueBlinkMs=millis(); s_valueBlink=!s_valueBlink; s_needRedraw=true;
  }
  if(s_sub==SUB_SLEEP_TIMER && s_sleepEditing && millis()-s_sleepBlinkMs>=450u) {
    s_sleepBlinkMs=millis(); s_sleepBlink=!s_sleepBlink; s_needRedraw=true; hal::display::markAllDirty();
  }
  if (s_sub == SUB_FILE_SYNC && millis() - s_fileSyncTickMs >= 300u) {
    s_fileSyncTickMs = millis(); s_needRedraw = true;
  }
  if (s_sub == SUB_UPLOADS) {
    // The service task schedules uploads. This page only snapshots their state.
    if(millis()-s_uploadTickMs>=450u) {
      s_uploadTickMs=millis(); s_uploadBlink=!s_uploadBlink;
      refreshUploadView(); s_needRedraw=true;
    }
  }
}

static ui::ListItem settingsMainItemFn(int idx, void*) {
    ui::ListItem item;
    idx -= s_updateNotice ? 1 : 0;
    if (idx == -1) { item.label="Update available"; item.opensScreen=true; item.fg=kUiFg; return item; }
    item.label = (idx >= 0 && idx < kRows) ? kLabels[idx] : "";
    item.opensScreen = idx == ROW_WIFI || idx == ROW_AGENT_STYLE || idx == ROW_UPLOADS || idx == ROW_SOUND ||
                       idx == ROW_SLEEP || idx == ROW_SENSORS || idx == ROW_ABOUT || idx == ROW_REPLAY_TUTORIAL;
    item.fg = kUiFg;
    if(s_valueEditing && idx==s_sel) {
      static char draft[40];
      if(idx==ROW_SERVER) snprintf(draft,sizeof(draft),"Server: %s",s_valueDraft?"Dev":"Prod");
      else if(idx==ROW_RECIPE_ALERTS) snprintf(draft,sizeof(draft),"Recipe alerts %s",s_valueDraft?"on":"off");
      else if(idx==ROW_OTA_CHANNEL) snprintf(draft,sizeof(draft),"Updates: %s",services::ota_client::releaseChannelName(static_cast<services::ota_client::ReleaseChannel>(s_valueDraft)));
      item.label=draft; item.editing=true; item.blinkOn=s_valueBlink;
    }
    return item;
}

static ui::ListItem settingsWifiMenuItemFn(int idx, void*) {
    ui::ListItem item;
    item.label = (idx >= 0 && idx < kWifiRows) ? kWifiLabels[idx] : "";
    item.fg = kUiFg;
    return item;
}

static ui::ListItem settingsSoundMenuItemFn(int idx, void*) {
    ui::ListItem item;
    item.label = (idx >= 0 && idx < kSoundRows) ? kSoundLabels[idx] : "";
    item.fg = kUiFg;
    return item;
}

static ui::ListItem settingsSleepMenuItemFn(int idx, void*) {
    ui::ListItem item;
    item.label = (idx >= 0 && idx < kSleepRows) ? kSleepLabels[idx] : "";
    item.opensScreen = idx == SLEEP_TIMER;
    if(s_valueEditing && idx==SLEEP_TOGGLE) {
      item.label=s_valueDraft?"Sleep: on":"Sleep: off";
      item.editing=true; item.blinkOn=s_valueBlink;
    }
    item.fg = kUiFg;
    return item;
}

static ui::ListItem settingsSensorsMenuItemFn(int idx, void*) {
    ui::ListItem item;
    item.label = (idx >= 0 && idx < kSensorRows) ? kSensorLabels[idx] : "";
    item.fg = kUiFg;
    return item;
}

static ui::ListItem settingsMusicItemFn(int idx, void* ctx) {
    const int chosen = ctx ? *static_cast<const int*>(ctx) : -1;
    ui::ListItem item;
    item.label   = (idx >= 0 && idx < kMusicOutRows) ? kMusicOutLabels[idx] : "";
    item.fg      = kUiFg;
    item.choice  = true;
    item.checked = idx == musicOutRowFromRoute(hal::audio::getOutputRoute());
    return item;
}

static ui::ListItem agentStyleItemFn(int idx, void*) {
    ui::ListItem item;
    item.label=ui::listening_style::kStyles[idx].name;
    item.fg=kUiFg; item.choice=true;
    item.checked=idx==ui::listening_style::selected();
    return item;
}

static ui::ListItem settingsWifiItemFn(int idx, void*) {
    const char* ssid = core::secrets::wifiNetworkSSID(idx);
    const bool live = hal::wifi::connected() && hal::wifi::getSTASSID() && ssid && ssid[0] &&
                      strcmp(hal::wifi::getSTASSID(), ssid) == 0;
    ui::ListItem item;
    item.label = (ssid && ssid[0]) ? ssid : "(empty)";
    item.fg    = live ? kUiAccent : kUiFg;
    return item;
}

static ui::ListItem settingsNetActionItemFn(int idx, void*) {
    ui::ListItem item;
    item.label = (idx >= 0 && idx < kNetActionRows) ? kNetActionLabels[idx] : "";
    item.opensScreen = false;
    if (idx == 0) {
        const char* ssid = core::secrets::wifiNetworkSSID(s_wifiNetSel);
        const bool live  = hal::wifi::connected() && hal::wifi::getSTASSID() && ssid && ssid[0] &&
                           strcmp(hal::wifi::getSTASSID(), ssid) == 0;
        item.label = live ? "Connected" : "Connect";
        item.fg    = live ? kUiAccent : kUiFg;
    } else {
        item.fg = kUiFg;
    }
    return item;
}

static void applySettingsMainListLayout() {
  s_mainList.rowH = settings_layout::kSettingsListRowH;
  s_mainList.rowPitch = settings_layout::kSettingsListRowPitch;
  s_mainList.topPad = settings_layout::kSettingsListTopPad;
  s_mainList.leftX = settings_layout::kSettingsListLeftX;
  s_mainList.scrollbarW = settings_layout::kSettingsListScrollbarW;
  s_mainList.scrollbarPad = settings_layout::kSettingsListScrollbarPad;
  s_mainList.scrollbarRight = settings_layout::kSettingsListScrollbarRight;
}

static void applySettingsSubListLayout(ui::ListView& list) {
  list.rowH = settings_layout::kSettingsSubListRowH;
  list.rowPitch = settings_layout::kSettingsSubListRowPitch;
  list.topPad = settings_layout::kSettingsSubListTopPad;
  list.leftX = settings_layout::kSettingsSubListLeftX;
  list.scrollbarW = settings_layout::kSettingsSubListScrollbarW;
  list.scrollbarPad = settings_layout::kSettingsSubListScrollbarPad;
  list.scrollbarRight = settings_layout::kSettingsSubListScrollbarRight;
}

static bool scrollSettingsList(ui::ListView& list, int& selected, int count, int steps) {
  list.totalItems = count;
  list.selected = selected;
  list.scroll(steps);
  if (list.selected == selected) return false;
  selected = list.selected;
  s_needRedraw = true;
  hal::display::markAllDirty();
  return true;
}

static void finishValueEdit() {
  if(!s_valueEditing) return;
  if(s_sub==SUB_SLEEP_MENU) {
    core::sleep_timer::setEnabled(s_valueDraft); updateSleepLabel();
  } else if(s_sel==ROW_RECIPE_ALERTS) {
    if(!services::automation::setRecipeAlertsEnabled(s_valueDraft)) {
      snprintf(s_recipeLabel,sizeof(s_recipeLabel),"Recipe alerts save failed");
    } else updateRecipeLabel();
  } else if(s_sel==ROW_SERVER) {
    services::agent_transport::selectBackendMode(s_valueDraft);
    updateServerLabel();
  } else if(s_sel==ROW_OTA_CHANNEL) {
    const bool ok = services::ota_client::setReleaseChannel(static_cast<services::ota_client::ReleaseChannel>(s_valueDraft));
    updateOtaChannelLabel();
    if (!ok) snprintf(s_otaChannelLabel, sizeof(s_otaChannelLabel), "Updates: %s", services::ota_client::lastError());
  }
  s_valueEditing=false; s_needRedraw=true;
}
static void beginValueEdit() {
  if (core::release_policy::productionOnly && s_sub != SUB_SLEEP_MENU &&
      s_sel == ROW_SERVER) {
    // These are informational rows on production images, not selectable modes.
    s_needRedraw = true;
    return;
  }
  s_valueDraft=s_sub==SUB_SLEEP_MENU?core::sleep_timer::enabled():
      s_sel==ROW_RECIPE_ALERTS?services::automation::recipeAlertsEnabled():
      s_sel==ROW_SERVER?core::secrets::isDevMode():
      static_cast<int>(services::ota_client::releaseChannel());
  s_valueEditing=true; s_valueBlink=true; s_valueBlinkMs=millis(); s_needRedraw=true;
  sfx::play(sfx::kSfxAlarmOpen);
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if (s_sub == SUB_FILE_SYNC && ui::nav::isEnterTap(ev)) { requestFileSync(); return; }
  if(s_sub==SUB_UPLOADS) {
    if(ev.type==ET::Scroll && ev.delta) {
      refreshUploadView();
      if(s_uploadEditing && s_uploadView.count) {
        const int count=s_uploadView.count;
        const int index=(s_uploadView.index+ev.delta%count+count)%count;
        s_uploadView=settingsUploadQueue().view(nullptr,index);
        strlcpy(s_uploadId,s_uploadView.item.id,sizeof(s_uploadId));
      } else s_uploadField=(s_uploadField+ev.delta%4+4)%4;
      s_uploadNotice[0]=0; s_needRedraw=true;
      sfx::play(sfx::kSfxAlarmNavigate); return;
    }
    if(ui::nav::isBackTap(ev) && s_uploadEditing) {
      s_uploadEditing=false; s_needRedraw=true; sfx::play(sfx::kSfxAlarmNavigate); return;
    }
    if(ui::nav::isEnterTap(ev)) {
      // Use the displayed stable ID; never substitute another item for a stale selection.
      if(s_uploadField==0) {
        refreshUploadView();
        if(s_uploadView.count) { s_uploadEditing=!s_uploadEditing; s_uploadBlink=true; }
        else uploadNotice("No queued items");
      } else if(s_uploadField==1) {
        const auto result=settingsUploadQueue().forceSend(s_uploadId);
        uploadNotice(result==UploadQueue::ForceResult::Scheduled?"Attempt requested":
                     result==UploadQueue::ForceResult::Busy?"Another force send\nis already pending":
                     result==UploadQueue::ForceResult::AlreadyUploading?"Already uploading":"Item no longer queued");
      } else if (s_uploadField == 2) {
        const bool reset=settingsUploadQueue().resetRetryHistory();
        uploadNotice(reset?"Retry history reset. Files kept.":"No queued items");
      } else {
        requestFileSync();
        ::scene::switchTo(&app, &kScreenSettingsFileSync);
      }
      sfx::play(s_uploadField==0?sfx::kSfxAlarmOpen:sfx::kSfxAlarmSave);
      s_needRedraw=true; return;
    }
  }
  if(s_valueEditing) {
    if(ev.type==ET::Scroll && ev.delta) {
      if (s_sub != SUB_SLEEP_MENU && s_sel == ROW_OTA_CHANNEL)
        s_valueDraft = (s_valueDraft + ev.delta % 3 + 3) % 3;
      else if(abs(ev.delta)%2) s_valueDraft=!s_valueDraft;
      sfx::play(sfx::kSfxAlarmNavigate); s_needRedraw=true;
    } else if(ui::nav::isEnterTap(ev) || ui::nav::isBackTap(ev)) finishValueEdit();
    return;
  }

  if (ev.type == ET::Scroll && ev.delta != 0) {
    const int steps = ev.delta;
    if(s_sub==SUB_ABOUT) {
      s_aboutList.scroll(steps); s_needRedraw=true; sfx::play(sfx::kSfxAlarmNavigate);
    } else if (s_sub == SUB_MAIN) {
      const int prev = s_sel;
      s_mainList.totalItems = kRows + (s_updateNotice ? 1 : 0);
      s_mainList.selected = s_sel + (s_updateNotice ? 1 : 0);
      s_mainList.scroll(steps);
      s_sel = s_mainList.selected - (s_updateNotice ? 1 : 0);
      if (prev != s_sel) { s_needRedraw = true; hal::display::markAllDirty(); }
    } else if (s_sub == SUB_WIFI_MENU) {
      scrollSettingsList(s_wifiMenuList, s_wifiMenuSel, kWifiRows, steps);
    } else if (s_sub == SUB_SOUND_MENU) {
      scrollSettingsList(s_soundMenuList, s_soundMenuSel, kSoundRows, steps);
    } else if (s_sub == SUB_AGENT_STYLE) {
      scrollSettingsList(s_agentStyleList,s_agentStyleSel,ui::listening_style::kCount,steps);
    } else if (s_sub == SUB_SLEEP_MENU) {
      scrollSettingsList(s_sleepMenuList, s_sleepMenuSel, kSleepRows, steps);
    } else if (s_sub == SUB_SLEEP_TIMER) {
      sfx::play(sfx::kSfxAlarmNavigate);
      if(!s_sleepEditing) { s_sleepField=(s_sleepField+steps%3+3)%3; s_needRedraw=true; hal::display::markAllDirty(); return; }
      s_sleepTimerDraft = static_cast<uint32_t>(constrain(
          static_cast<int>(s_sleepTimerDraft) + steps * (s_sleepField==0?60:1),
          static_cast<int>(core::sleep_timer::kMinSeconds),
          static_cast<int>(core::sleep_timer::kMaxSeconds)));
      s_sleepSaveFailed = false;
      s_needRedraw = true;
      hal::display::markAllDirty();
    } else if (s_sub == SUB_SENSORS_MENU) {
      scrollSettingsList(s_sensorsMenuList, s_sensorsMenuSel, kSensorRows, steps);
    } else if (s_sub == SUB_SFX_VOLUME) {
      const int next = constrain(sfx::volume() - steps * 5, 0, 100);
      if (next != sfx::volume()) {
        sfx::setVolume(next);
        services::vibration::navigationFeedback(services::vibration::Cue::Scroll);
        sfx::play(sfx::kSfxMenuTick);
        s_needRedraw = true;
        hal::display::markAllDirty();
      }
    } else if (s_sub == SUB_MUSIC_OUTPUT) {
      s_musicList.totalItems = kMusicOutRows;
      s_musicList.selected   = s_musicOutSel;
      s_musicList.scroll(steps);
      if (s_musicList.selected != s_musicOutSel) {
        s_musicOutSel = s_musicList.selected;
        // Browsing does not change the active route; C applies the highlighted row.
        s_needRedraw = true;
        hal::display::markAllDirty();
      }
    } else if (s_sub == SUB_WIFI_NETS) {
      const int n = core::secrets::wifiNetworkCount();
      if (n > 0) {
        s_wifiList.totalItems = n;
        s_wifiList.selected   = s_wifiNetSel;
        s_wifiList.scroll(steps);
        s_wifiNetSel = s_wifiList.selected;
        s_needRedraw = true;
        hal::display::markAllDirty();
      }
    } else if (s_sub == SUB_WIFI_NET_ACTION) {
      s_netActionList.totalItems = kNetActionRows;
      s_netActionList.selected   = s_netActionSel;
      s_netActionList.scroll(steps);
      s_netActionSel = s_netActionList.selected;
      s_needRedraw = true;
      hal::display::markAllDirty();
    }
    return;
  }

  if (s_sub == SUB_SLEEP_TIMER && ev.type == ET::ButtonB_Tap) {
    s_sleepTimerDraft = core::sleep_timer::kDefaultSeconds;
    s_sleepSaveFailed = false;
    s_needRedraw = true;
    hal::display::markAllDirty();
    return;
  }

  if (ui::nav::isEnterTap(ev)) {
    if(s_sub==SUB_AGENT_STYLE) {
      s_agentStyleSaveFailed=!ui::listening_style::select(s_agentStyleSel);
      if(!s_agentStyleSaveFailed) {
        sfx::play(sfx::kSfxAlarmSave);
        NavHandler::instance().popToRoot();
      } else { s_needRedraw=true; hal::display::markAllDirty(); }
      return;
    }
    if (s_sub == SUB_SLEEP_TIMER) {
      if(s_sleepField<2) { sfx::play(sfx::kSfxAlarmNavigate); s_sleepEditing=!s_sleepEditing; s_sleepBlink=true; s_sleepBlinkMs=millis(); s_needRedraw=true; hal::display::markAllDirty(); return; }
      if (core::sleep_timer::save(s_sleepTimerDraft)) {
        showSleepMenu();
      } else {
        s_sleepSaveFailed = true;
        s_needRedraw = true;
        hal::display::markAllDirty();
      }
      return;
    }
    if (s_sub == SUB_WIFI_MENU) {
      if (s_wifiMenuSel == WIFI_QR || s_wifiMenuSel == WIFI_HELP) {
        s_resumeSub = SUB_WIFI_MENU;
        s_preserveMainSelection = true;
        if(s_wifiMenuSel == WIFI_QR) NavHandler::instance().push(&apps::wifi_setup::app);
        else apps::tutorial::beginWifiSetup();
      } else {
        showWifiNetworks(true);
      }
      return;
    }
    if (s_sub == SUB_SOUND_MENU) {
      if (s_soundMenuSel == SOUND_MUSIC_OUTPUT) {
        showMusicOutput();
      } else if (s_soundMenuSel == SOUND_SFX_VOLUME) {
        showSfxVolume();
      }
      return;
    }
    if (s_sub == SUB_SLEEP_MENU) {
      if (s_sleepMenuSel == SLEEP_TOGGLE) {
        beginValueEdit();
      } else if (s_sleepMenuSel == SLEEP_TIMER) {
        showSleepTimer();
      } else if (s_sleepMenuSel == SLEEP_DEFAULT) {
        s_sleepSaveFailed = !core::sleep_timer::save(core::sleep_timer::kDefaultSeconds);
        updateSleepLabel();
        s_needRedraw = true;
        hal::display::markAllDirty();
      } else {
        services::doBatterySleep();
      }
      return;
    }
    if (s_sub == SUB_SENSORS_MENU) {
      s_resumeSub = SUB_SENSORS_MENU;
      s_preserveMainSelection = true;
      if (s_sensorsMenuSel == SENSOR_IMU_CALIBRATION) {
        NavHandler::instance().push(&apps::imu_calibration_app::app);
      } else {
        NavHandler::instance().push(&apps::hardware_test::app);
      }
      return;
    }
    if (s_sub == SUB_MUSIC_OUTPUT) {
      services::audio_output::selectAndSave(musicOutRouteFromRow(s_musicOutSel));
      s_needRedraw=true; sfx::play(sfx::kSfxAlarmSave);
      return;
    }
    if (s_sub == SUB_SFX_VOLUME) {
      sfx::saveVolume();
      showSoundMenu();
      return;
    }
    if (s_sub == SUB_WIFI_NETS) {
      const int n = core::secrets::wifiNetworkCount();
      if (n <= 0 || s_wifiNetSel < 0 || s_wifiNetSel >= n) return;
      showNetworkAction();
      return;
    }
    if (s_sub == SUB_WIFI_NET_ACTION) {
      const int n = core::secrets::wifiNetworkCount();
      if (s_wifiNetSel < 0 || s_wifiNetSel >= n) {
        showWifiNetworks(false);
        return;
      }
      if (s_netActionSel == 0) {
        // Connect — shared deferred (Core 0) bring-up + join path. Calling
        // hal::wifi::connect() inline here (UI core) fails to re-init the radio
        // off Core 0; connectSaved() routes it through the wifi_mgr task.
        const char* ssid = core::secrets::wifiNetworkSSID(s_wifiNetSel);
        services::wifi_provision::connectSaved(ssid);
      } else {
        // Delete
        (void)services::wifi_provision::forgetSaved(s_wifiNetSel);
        if (s_wifiNetSel >= core::secrets::wifiNetworkCount()) {
          s_wifiNetSel = core::secrets::wifiNetworkCount() - 1;
          if (s_wifiNetSel < 0) s_wifiNetSel = 0;
        }
        s_wifiList.selected = s_wifiNetSel;
      }
      showWifiNetworks(false);
      return;
    }
    if(s_sub == SUB_ABOUT) return;
    switch (s_sel) {
      case ROW_WIFI:
        showWifiMenu();
        return;
      case ROW_UPLOADS:
        showUploads();
        return;
      case ROW_SOUND:
        showSoundMenu();
        return;
      case ROW_RECIPE_ALERTS: beginValueEdit(); return;
      case ROW_AGENT_STYLE:
        showAgentStyle();
        return;
      case ROW_REPLAY_TUTORIAL:
        apps::tutorial::replay();
        return;
      case ROW_SERVER: beginValueEdit(); return;
      case ROW_SLEEP:
        showSleepMenu();
        return;
      case ROW_SENSORS:
        showSensorsMenu();
        return;
      case ROW_OTA_CHANNEL: beginValueEdit(); return;
      case -1: // Conditional update notice uses the existing signed installer.
      case ROW_UPDATE_FW:
        s_resumeSub = SUB_MAIN;
        s_preserveMainSelection = true;
        NavHandler::instance().push(&apps::ota_update::app);
        return;
      case ROW_REBOOT:
        ESP.restart();
        return;
      case ROW_ABOUT:
        showAbout();
        return;
      default:
        return;
    }
  }

  if (ui::nav::isBackTap(ev)) {
    if (s_sub == SUB_FILE_SYNC) { showUploads(); return; }
    if (s_sub == SUB_SLEEP_TIMER) {
      if(s_sleepEditing) { s_sleepEditing=false; s_needRedraw=true; hal::display::markAllDirty(); return; }
      showSleepMenu();
      return;
    }
    if (s_sub == SUB_WIFI_NET_ACTION) {
      showWifiNetworks(false);
      return;
    }
    if (s_sub == SUB_WIFI_NETS) {
      showWifiMenu();
      return;
    }
    if (s_sub == SUB_MUSIC_OUTPUT) { showSoundMenu(); return; }
    if (s_sub == SUB_SFX_VOLUME) {
      sfx::saveVolume();
      showSoundMenu();
      return;
    }
    if (s_sub != SUB_MAIN) {
      showMain();
      return;
    }
    NavHandler::instance().pop();
  }
}

static void buildSettingsScene(ui::scene::Scene& scene, int16_t width, int16_t height, Sub sub) {
  const bool physicalSurface = width==ui::safe_area::kWidth && height==ui::safe_area::kHeight;
  scene.clear();
  ui::scene::Builder b(scene, width, height);
  if (sub != SUB_MAIN) b.clearContent(kUiBg);

  switch (sub) {
    case SUB_MAIN: {
      scene.addRect("content.bg",
                    {settings_layout::kSettingsContentBgX,
                     settings_layout::kSettingsContentBgY,
                     settings_layout::kSettingsContentBgW,
                     settings_layout::kSettingsContentBgH},
                    kUiBg, settings_layout::kSettingsContentBgRadius,
                    settings_layout::kSettingsContentBgZ);
      applySettingsMainListLayout();
      s_mainList.totalItems = kRows + (s_updateNotice ? 1 : 0);
      s_mainList.selected = s_sel + (s_updateNotice ? 1 : 0);
      s_mainList.buildScene(scene,
                            settings_layout::kSettingsListAreaX,
                            settings_layout::kSettingsListAreaY,
                            settings_layout::kSettingsListAreaW,
                            settings_layout::kSettingsListAreaH,
                            settingsMainItemFn, nullptr,
                            kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);
      break;
    }
    case SUB_WIFI_MENU: {

      applySettingsSubListLayout(s_wifiMenuList);
      s_wifiMenuList.totalItems = kWifiRows;
      s_wifiMenuList.selected = s_wifiMenuSel;
      s_wifiMenuList.buildScene(scene,
                                settings_layout::kSettingsSubListAreaX,
                                settings_layout::kSettingsSubListAreaY,
                                settings_layout::kSettingsSubListAreaW,
                                settings_layout::kSettingsSubListAreaH,
                                settingsWifiMenuItemFn, nullptr,
                                kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);
      break;
    }
    case SUB_SOUND_MENU: {

      applySettingsSubListLayout(s_soundMenuList);
      s_soundMenuList.totalItems = kSoundRows;
      s_soundMenuList.selected = s_soundMenuSel;
      s_soundMenuList.buildScene(scene,
                                 settings_layout::kSettingsSubListAreaX,
                                 settings_layout::kSettingsSubListAreaY,
                                 settings_layout::kSettingsSubListAreaW,
                                 settings_layout::kSettingsSubListAreaH,
                                 settingsSoundMenuItemFn, nullptr,
                                 kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);
      break;
    }
    case SUB_MUSIC_OUTPUT: {

      applySettingsSubListLayout(s_musicList);
      s_musicList.totalItems = kMusicOutRows;
      s_musicList.selected   = s_musicOutSel;
      s_musicList.buildScene(scene,
                             settings_layout::kSettingsSubListAreaX,
                             settings_layout::kSettingsSubListAreaY,
                             settings_layout::kSettingsSubListAreaW,
                             settings_layout::kSettingsSubListAreaH,
                             settingsMusicItemFn, &s_musicOutSel,
                             kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);

      break;
    }
    case SUB_AGENT_STYLE: {
      scene.addLabel("agent-style.title",{16,43,(int16_t)(width-32),20},"Agent UI",kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
      applySettingsSubListLayout(s_agentStyleList);
      s_agentStyleList.totalItems=ui::listening_style::kCount;
      s_agentStyleList.selected=s_agentStyleSel;
      s_agentStyleList.buildScene(scene,16,68,width-32,126,agentStyleItemFn,nullptr,
          kUiSelBg,kUiSelFg,kUiBg,kUiPanel,kUiFg);
      scene.addLabel("agent-style.hint",{12,199,(int16_t)(width-24),16},
          s_agentStyleSaveFailed?"Save failed. Press C to retry":"Turn: browse / C: use + home",
          kUiFg,kUiBg,1,ui::scene::TextAlign::MiddleCenter,2);
      break;
    }
    case SUB_SFX_VOLUME: {
      char value[20];
      snprintf(value, sizeof(value), "Volume: %d%%", sfx::volume());

      scene.addLabel("sfx.title",{14,52,(int16_t)(width-28),24},"Sound effects",kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
      scene.addLabel("sfx.value",{14,92,(int16_t)(width-28),40},value,kUiFg,kUiBg,3,ui::scene::TextAlign::MiddleCenter,2);
      scene.addProgressBar("sfx.volume",{24,154,(int16_t)(width-48),12},sfx::volume(),100,kUiPanel,kUiAccent,3,2);

      break;
    }
    case SUB_WIFI_NETS: {
      const int n = core::secrets::wifiNetworkCount();

      if (n == 0) {
        b.statusLine("wifi.empty.0", "No saved networks.", kUiMuted);
        b.statusLine("wifi.empty.1", "Use 'WiFi Setup' to add one.", kUiMuted);
      } else {
        applySettingsSubListLayout(s_wifiList);
        s_wifiList.totalItems = n;
        s_wifiList.selected = s_wifiNetSel;
        s_wifiList.buildScene(scene,
                              settings_layout::kSettingsSubListAreaX,
                              settings_layout::kSettingsSubListAreaY,
                              settings_layout::kSettingsSubListAreaW,
                              settings_layout::kSettingsSubListAreaH,
                              settingsWifiItemFn, nullptr,
                              kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);
      }
      break;
    }
    case SUB_WIFI_NET_ACTION: {
      const char* ssid = core::secrets::wifiNetworkSSID(s_wifiNetSel);
      b.panelHeader("network.title", ssid && ssid[0] ? ssid : "Network");
      applySettingsSubListLayout(s_netActionList);
      s_netActionList.totalItems = kNetActionRows;
      s_netActionList.selected   = s_netActionSel;
      s_netActionList.buildScene(scene,
                                 settings_layout::kSettingsSubListAreaX,
                                 settings_layout::kSettingsSubListAreaY,
                                 settings_layout::kSettingsSubListAreaW,
                                 settings_layout::kSettingsSubListAreaH,
                                 settingsNetActionItemFn, nullptr,
                                 kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);
      break;
    }
    case SUB_SLEEP_MENU: {

      applySettingsSubListLayout(s_sleepMenuList);
      s_sleepMenuList.totalItems = kSleepRows;
      s_sleepMenuList.selected = s_sleepMenuSel;
      s_sleepMenuList.buildScene(scene,
                                 settings_layout::kSettingsSubListAreaX,
                                 settings_layout::kSettingsSubListAreaY,
                                 settings_layout::kSettingsSubListAreaW,
                                 settings_layout::kSettingsSubListAreaH,
                                 settingsSleepMenuItemFn, nullptr,
                                 kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);

      break;
    }
    case SUB_SLEEP_TIMER: {
      char minutes[8],seconds[8];
      snprintf(minutes,sizeof(minutes),"%02lu",(unsigned long)(s_sleepTimerDraft/60));
      snprintf(seconds,sizeof(seconds),"%02lu",(unsigned long)(s_sleepTimerDraft%60));
      scene.addLabel("sleep.caption",{14,48,(int16_t)(width-28),24},"Minutes : Seconds",kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
      ui::editField(scene,physicalSurface,"sleep.minutes",{(int16_t)(width/2-72),90,60,44},minutes,s_sleepField==0,s_sleepEditing,s_sleepBlink,4);
      scene.addLabel("sleep.colon",{(int16_t)(width/2-12),90,24,44},":",kUiFg,kUiBg,4,ui::scene::TextAlign::MiddleCenter,2);
      ui::editField(scene,physicalSurface,"sleep.seconds",{(int16_t)(width/2+12),90,60,44},seconds,s_sleepField==1,s_sleepEditing,s_sleepBlink,4);
      ui::editField(scene,physicalSurface,"sleep.save",{14,166,(int16_t)(width-28),32},"Save",s_sleepField==2,false,true);
      if(s_sleepSaveFailed) scene.addLabel("sleep.error",{14,206,(int16_t)(width-28),20},"Save failed. Retry.",kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
      break;
    }
    case SUB_SENSORS_MENU: {

      applySettingsSubListLayout(s_sensorsMenuList);
      s_sensorsMenuList.totalItems = kSensorRows;
      s_sensorsMenuList.selected = s_sensorsMenuSel;
      s_sensorsMenuList.buildScene(scene,
                                   settings_layout::kSettingsSubListAreaX,
                                   settings_layout::kSettingsSubListAreaY,
                                   settings_layout::kSettingsSubListAreaW,
                                   settings_layout::kSettingsSubListAreaH,
                                   settingsSensorsMenuItemFn, nullptr,
                                   kUiSelBg, kUiSelFg, kUiBg, kUiPanel, kUiFg);
      break;
    }
    case SUB_ABOUT: {
      const auto account=services::account_info::snapshot();
      char rows[6][128];
      snprintf(rows[0],sizeof(rows[0]),"Firmware: %s",OTA_FIRMWARE_VERSION);
      snprintf(rows[1],sizeof(rows[1]),"Account: %s",account.name);
      snprintf(rows[2],sizeof(rows[2]),"Build: %s",services::ota_client::buildKindName());
      snprintf(rows[3],sizeof(rows[3]),"Board: %s",OTA_HARDWARE_ID);
      snprintf(rows[4],sizeof(rows[4]),"Updates: Ask first");
      snprintf(rows[5],sizeof(rows[5]),"API key: %s",core::secrets::backendConfig().apiKey[0]?"Configured":"Not set");
      applySettingsSubListLayout(s_aboutList);s_aboutList.totalItems=6;
      s_aboutList.buildScene(scene,settings_layout::kSettingsSubListAreaX,
          settings_layout::kSettingsSubListAreaY,settings_layout::kSettingsSubListAreaW,
          settings_layout::kSettingsSubListAreaH,
          [](int index,void* data)->ui::ListItem {ui::ListItem item;item.label=static_cast<char(*)[128]>(data)[index];item.fg=kUiFg;item.opensScreen=false;return item;},rows,
          kUiSelBg,kUiSelFg,kUiBg,kUiPanel,kUiFg);
      break;
    }
    case SUB_FILE_SYNC: {
      services::device_fs::SyncStatus status;
      services::device_fs::getSyncStatus(status);
      b.panelHeader("Sync to web");
      if (s_fileSyncError) b.statusLine("sync.error", s_fileSyncError, kUiMuted);
      else if (!hal::wifi::connected()) b.statusLine("sync.wifi", "Waiting for WiFi", kUiMuted);
      else if (!core::secrets::hasDeviceToken()) b.statusLine("sync.auth", "Registering device...", kUiMuted);
      else {
        b.statusLine("sync.phase", status.phase, kUiFg);
        b.statusLine("sync.detail", status.detail, kUiMuted);
        b.progressBar("sync.progress", status.value, status.max);
      }
      b.footer("sync.footer", "C: Sync   LB: Back");
      break;
    }
    case SUB_UPLOADS: {
      refreshUploadView();
      const auto& v=s_uploadView;
      char header[40],name[32],detail[80];
      snprintf(header,sizeof(header),"%d queued | %s",v.count,hal::wifi::connected()?"Online":"Offline");
      scene.addLabel("uploads.summary",{14,46,(int16_t)(width-28),22},header,kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
      if(v.index<0) strlcpy(name,"No queued items",sizeof(name));
      else if(strlen(v.item.fileName)>20) snprintf(name,sizeof(name),"%.17s...",v.item.fileName);
      else strlcpy(name,v.item.fileName,sizeof(name));
      ui::editField(scene,physicalSurface,"uploads.item",{14,76,(int16_t)(width-28),28},name,s_uploadField==0,s_uploadEditing,s_uploadBlink);
      if(s_uploadNotice[0] && millis()-s_uploadNoticeMs<3500u) strlcpy(detail,s_uploadNotice,sizeof(detail));
      else if(s_uploadEditing) snprintf(detail,sizeof(detail),"Item %d of %d\nTurn to pick. C: Done",v.index+1,v.count);
      else if(!v.count) strlcpy(detail,"New recordings and\nphotos appear here",sizeof(detail));
      else if(v.item.status==QSTATUS_UPLOADING) strlcpy(detail,v.resetPending?"Uploading; reset\nafter completion":"Uploading to Agent",sizeof(detail));
      else if(v.paused) strlcpy(detail,"Waiting for device\nto finish audio/I/O",sizeof(detail));
      else if(!hal::wifi::connected()) strlcpy(detail,"Waiting for Wi-Fi",sizeof(detail));
      else if(v.forced) strlcpy(detail,"Force send scheduled",sizeof(detail));
      else if(v.waitMs) snprintf(detail,sizeof(detail),"Retry in %lum %lus\n%s",
          (unsigned long)(v.waitMs/60000u),(unsigned long)((v.waitMs/1000u)%60),v.item.lastError);
      else snprintf(detail,sizeof(detail),"Ready | %u attempts\n%s",v.item.attempts,v.item.lastError);
      ui::morning_style::text(scene,"uploads.detail",detail,{14,108,(int16_t)(width-28),40},2);
      const bool flash=s_uploadNotice[0] && millis()-s_uploadNoticeMs<500u;
      // Scroll the two action slots to reveal full file sync below queue reset.
      if (s_uploadField < 3)
        ui::editField(scene,physicalSurface,"uploads.force",{14,154,(int16_t)(width-28),28},"Force send item",s_uploadField==1,flash,true);
      ui::editField(scene,physicalSurface,"uploads.reset",{14,(int16_t)(s_uploadField==3?154:190),(int16_t)(width-28),28},"Force reset queue",s_uploadField==2,flash,true);
      if (s_uploadField == 3)
        ui::editField(scene,physicalSurface,"uploads.sync",{14,190,(int16_t)(width-28),28},"Sync to web",true,false,true);
      break;
    }
  }

}

static void buildMainScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_MAIN);
}

static void buildWifiMenuScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_WIFI_MENU);
}

static void buildUploadsScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_UPLOADS);
}

static void buildSoundMenuScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_SOUND_MENU);
}

static void buildAgentStyleScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene,width,height,SUB_AGENT_STYLE);
}

static void buildMusicScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_MUSIC_OUTPUT);
}

static void buildSfxVolumeScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_SFX_VOLUME);
}

static void buildWifiScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_WIFI_NETS);
}

static void buildNetworkActionScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_WIFI_NET_ACTION);
}

static void buildSleepMenuScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_SLEEP_MENU);
}

static void buildSleepTimerScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_SLEEP_TIMER);
}

static void buildSensorsMenuScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_SENSORS_MENU);
}

static void buildAboutScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildSettingsScene(scene, width, height, SUB_ABOUT);
}

void onDraw(TFT_eSprite& canvas) {
  if (!s_needRedraw) {
    if (s_sub == SUB_AGENT_STYLE) ui::listening_style::restorePreviewChrome(canvas,s_agentStyleSel);
    return;
  }
  s_needRedraw = false;

  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildSettingsScene(scene, canvas.width(), canvas.height(), s_sub);
  if (s_sub == SUB_AGENT_STYLE) canvas.fillSprite(kUiBg);
  scene.render(canvas);
  if (s_sub == SUB_AGENT_STYLE) {
    ui::listening_style::drawPreview(canvas, s_agentStyleSel);
    NavHandler::instance().invalidateChrome();
  }
  hal::display::markAllDirty();
}

bool onChord() {
  if (s_sub == SUB_FILE_SYNC) { showUploads(); return true; }
  if(s_sub==SUB_UPLOADS && s_uploadEditing) { s_uploadEditing=false; s_needRedraw=true; return true; }
  if(s_valueEditing) { finishValueEdit(); return true; }
  if (s_sub == SUB_SLEEP_TIMER) {
    showSleepMenu();
    return true;
  }
  if (s_sub == SUB_WIFI_NET_ACTION) {
    showWifiNetworks(false);
    return true;
  }
  if (s_sub == SUB_WIFI_NETS) {
    showWifiMenu();
    return true;
  }
  if (s_sub == SUB_MUSIC_OUTPUT) {
    showSoundMenu();
    return true;
  }
  if (s_sub == SUB_SFX_VOLUME) {
    sfx::saveVolume();
    showSoundMenu();
    return true;
  }
  if (s_sub != SUB_MAIN) {
    showMain();
    return true;
  }
  return false;
}

static void enterWifiMenu() {
  s_sub = SUB_WIFI_MENU;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterUploads() {
  s_sub = SUB_UPLOADS;
  s_uploadField=0; s_uploadEditing=false; s_uploadId[0]=0; s_uploadNotice[0]=0;
  s_uploadView.index=0; s_uploadTickMs=millis(); refreshUploadView();
  // This screen observes connectivity. wifi_mgr owns joins, retry backoff,
  // and deferred restores after camera/photos; opening it must not restart STA.
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterSoundMenu() {
  s_sub = SUB_SOUND_MENU;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterMusicOutput() {
  s_sub = SUB_MUSIC_OUTPUT;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterAgentStyle() {
  s_sub=SUB_AGENT_STYLE;
  s_agentStyleSel=ui::listening_style::selected();
  s_agentStyleSaveFailed=false;
  s_agentStyleList.scrollFirst=0;
  s_needRedraw=true; hal::display::markAllDirty();
}

static void enterSfxVolume() {
  s_sub = SUB_SFX_VOLUME;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterWifiNetworks() {
  s_sub = SUB_WIFI_NETS;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterNetworkAction() {
  s_sub = SUB_WIFI_NET_ACTION;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterSleepMenu() {
  s_sub = SUB_SLEEP_MENU;
  s_sleepSaveFailed = false;
  updateSleepLabel();
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterSleepTimer() {
  s_sub = SUB_SLEEP_TIMER;
  s_sleepTimerDraft = core::sleep_timer::seconds();
  s_sleepEditing=false; s_sleepField=0; s_sleepBlink=true;
  s_sleepSaveFailed = false;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterSensorsMenu() {
  s_sub = SUB_SENSORS_MENU;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void enterAbout() {
  s_sub = SUB_ABOUT; s_aboutList.selected=0;s_aboutList.scrollFirst=0;s_aboutList.totalItems=6;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void screenTick(uint32_t) {
  tick();
}

const ::scene::Screen kScreenSettingsMain = {
    "Main", enterMainView, nullptr, screenTick, onInput, onChord, onDraw,
    "main", buildMainScene,
    &screen_chrome_layout::kSettingsMainTopBarColor,
};

const ::scene::Screen kScreenSettingsWifiMenu = {
    "WiFi", enterWifiMenu, nullptr, nullptr, onInput, onChord, onDraw,
    "wifi-menu", buildWifiMenuScene,
};

const ::scene::Screen kScreenSettingsUploads = {
    "Uploads", enterUploads, nullptr, screenTick, onInput, onChord, onDraw,
    "uploads", buildUploadsScene,
    &screen_chrome_layout::kSettingsUploadsTopBarColor,
};

static void enterFileSync() { s_sub = SUB_FILE_SYNC; s_needRedraw = true; }
static void buildFileSyncScene(ui::scene::Scene& graph, int16_t width, int16_t height) {
  buildSettingsScene(graph, width, height, SUB_FILE_SYNC);
}
const ::scene::Screen kScreenSettingsFileSync = {
    "Sync to web", enterFileSync, nullptr, screenTick, onInput, onChord, onDraw,
    "sync", buildFileSyncScene,
    &screen_chrome_layout::kSettingsUploadsTopBarColor,
};

const ::scene::Screen kScreenSettingsSoundMenu = {
    "Sound", enterSoundMenu, nullptr, nullptr, onInput, onChord, onDraw,
    "sound-menu", buildSoundMenuScene,
};

const ::scene::Screen kScreenSettingsAgentStyle = {
    "Agent UI", enterAgentStyle, [](){ui::listening_style::exitPreview();}, nullptr, onInput, onChord, onDraw,
    "agent-style", buildAgentStyleScene,
};

const ::scene::Screen kScreenSettingsMusic = {
    "Music Output", enterMusicOutput, nullptr, nullptr, onInput, onChord, onDraw,
    "music", buildMusicScene,
    &screen_chrome_layout::kSettingsMusicTopBarColor,
};

const ::scene::Screen kScreenSettingsSfxVolume = {
    "SFX Volume", enterSfxVolume, nullptr, nullptr, onInput, onChord, onDraw,
    "sfx-volume", buildSfxVolumeScene,
};

const ::scene::Screen kScreenSettingsWifi = {
    "Saved Networks", enterWifiNetworks, nullptr, nullptr, onInput, onChord, onDraw,
    "wifi", buildWifiScene,
    &screen_chrome_layout::kSettingsWifiTopBarColor,
};

const ::scene::Screen kScreenSettingsNetworkAction = {
    "Network Action", enterNetworkAction, nullptr, nullptr, onInput, onChord, onDraw,
    "network-action", buildNetworkActionScene,
    &screen_chrome_layout::kSettingsNetworkActionTopBarColor,
};

const ::scene::Screen kScreenSettingsSleepMenu = {
    "Sleep", enterSleepMenu, nullptr, screenTick, onInput, onChord, onDraw,
    "sleep-menu", buildSleepMenuScene,
};

const ::scene::Screen kScreenSettingsSleepTimer = {
    "Sleep Timer", enterSleepTimer, nullptr, [](uint32_t) { tick(); }, onInput, onChord, onDraw,
    "sleep-timer", buildSleepTimerScene,
};

static void showSleepTimer() {
  ::scene::switchTo(&app, &kScreenSettingsSleepTimer);
}

const ::scene::Screen kScreenSettingsSensorsMenu = {
    "Sensors", enterSensorsMenu, nullptr, nullptr, onInput, onChord, onDraw,
    "sensors-menu", buildSensorsMenuScene,
};

const ::scene::Screen kScreenSettingsAbout = {
      "About", enterAbout, nullptr, screenTick, onInput, onChord, onDraw,
    "about", buildAboutScene,
    &screen_chrome_layout::kSettingsAboutTopBarColor,
};

static void showMain() {
  services::uploads::ensureInit();
  s_preserveMainSelection = true;
  ::scene::switchTo(&app, &kScreenSettingsMain, ::scene::ScreenTransition::Back);
}

static void showWifiMenu() {
  ::scene::switchTo(&app, &kScreenSettingsWifiMenu, s_sub != SUB_MAIN ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

static void showUploads() {
  ::scene::switchTo(&app, &kScreenSettingsUploads, s_sub == SUB_FILE_SYNC ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

static void showSoundMenu() {
  ::scene::switchTo(&app, &kScreenSettingsSoundMenu, s_sub != SUB_MAIN ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

static void showAgentStyle() {
  ::scene::switchTo(&app,&kScreenSettingsAgentStyle);
}

void openWifiMenu() {
  if(NavHandler::instance().top()!=&app) NavHandler::instance().push(&app);
  showWifiMenu();
}

void openAgentStyles() {
  if(NavHandler::instance().top()!=&app) NavHandler::instance().push(&app);
  showAgentStyle();
}

static void showMusicOutput() {
  s_musicOutSel = musicOutRowFromRoute(hal::audio::getOutputRoute());
  ::scene::switchTo(&app, &kScreenSettingsMusic);
}

static void showSfxVolume() {
  sfx::loadVolume();
  ::scene::switchTo(&app, &kScreenSettingsSfxVolume);
}

static void showWifiNetworks(bool resetSelection) {
  if (resetSelection) s_wifiNetSel = 0;
  ::scene::switchTo(&app, &kScreenSettingsWifi, s_sub == SUB_WIFI_NET_ACTION ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

static void showNetworkAction() {
  s_netActionSel = 0;
  ::scene::switchTo(&app, &kScreenSettingsNetworkAction);
}

static void showSleepMenu() {
  ::scene::switchTo(&app, &kScreenSettingsSleepMenu, s_sub == SUB_SLEEP_TIMER ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

static void showSensorsMenu() {
  ::scene::switchTo(&app, &kScreenSettingsSensorsMenu);
}

static void showAbout() {
  ::scene::switchTo(&app, &kScreenSettingsAbout);
}

static const ::scene::Screen* const kScreens[] = {
    &kScreenSettingsMain,
    &kScreenSettingsWifiMenu,
    &kScreenSettingsSoundMenu,
    &kScreenSettingsAgentStyle,
    &kScreenSettingsMusic,
    &kScreenSettingsSfxVolume,
    &kScreenSettingsWifi,
    &kScreenSettingsNetworkAction,
    &kScreenSettingsSleepMenu,
    &kScreenSettingsSleepTimer,
    &kScreenSettingsSensorsMenu,
    &kScreenSettingsUploads,
    &kScreenSettingsFileSync,
    &kScreenSettingsAbout,
};
static AppPresentation presentationPolicy() {
  AppPresentation p{};
  if (app.currentScreen == &kScreenSettingsAgentStyle) {
    p.contentUsesStatusArea = true;
    p.statusBar = AppPresentation::StatusBar::GlyphOverlay;
  }
  return p;
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Settings", "settings", &kScreenSettingsMain,
                            prepareWorkflow, prepareWorkflow, exitApp,
                            "src/apps/settings_menu_app.cpp", kScreens, presentationPolicy, nullptr);

}  // namespace settings_menu
}  // namespace apps
