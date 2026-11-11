#include "ui/screen_chrome.h"

#include <Arduino.h>
#include <string.h>
#include "apps/wifi_setup_app.h"

#include "config/boot_config.h"
#include "config/screen_chrome_layout.h"
#include "config/ui_theme.h"
#include "ui/color.h"

namespace {
const void* s_screen = nullptr;
bool s_hasSceneBackground = false;
uint16_t s_sceneBackground = kUiBg;
bool s_repaintedBar = false;

struct ScreenChromeEntry {
    const char* app;
    const char* screen;
    uint16_t color;
};

using namespace screen_chrome_layout;

static constexpr ScreenChromeEntry kEntries[] = {
    {"Boot", "BootLogo", kBootBootTopBarColor},
    {"Boot", "BootPresents", kBootPresentsTopBarColor},
    {"Boot", "BootEyeMorph", kBootEyesTopBarColor},
    {"Boot", "BootHandoff", kBootHandoffTopBarColor},
    {"Menu", "Menu", kMenuHomeTopBarColor},
    {"Kami", "Eyes", kKamiEyesTopBarColor},
    {"Kami", "Recording", kKamiRecordingTopBarColor},
    {"Kami", "Thinking", kKamiThinkingTopBarColor},
    {"Kami", "Reply", kKamiReplyTopBarColor},
    {"Kami", "Error", kKamiErrorTopBarColor},
    {"Camera", "Viewfinder", kCameraViewfinderTopBarColor},
    {"Camera", "Settings", kCameraSettingsTopBarColor},
    {"Camera", "Error", kCameraErrorTopBarColor},
    {"Settings", "Main", kSettingsMainTopBarColor},
    {"Settings", "Uploads", kSettingsUploadsTopBarColor},
    {"Settings", "Music Output", kSettingsMusicTopBarColor},
    {"Settings", "Saved Networks", kSettingsWifiTopBarColor},
    {"Settings", "Network Action", kSettingsNetworkActionTopBarColor},
    {"Settings", "About", kSettingsAboutTopBarColor},
    {"Music", "Browser", kMusicBrowserTopBarColor},
    {"Music", "Now Playing", kMusicNowPlayingTopBarColor},
    {"Audio Recordings", "Browser", kMusicBrowserTopBarColor},
    {"Audio Recordings", "Now Playing", kMusicNowPlayingTopBarColor},
    {"Steps", "Steps", kStepsMainTopBarColor},
    {"Tetris", "Tetris", kTetrisGameTopBarColor},
    {"Hardware Test", "Hardware Test", kHardwareTestTopBarColor},
    {"WiFi Portal", "WiFi Portal", kWifiPortalTopBarColor},
    {"WiFi Setup", "WiFi Setup", kWifiSetupQrScanTopBarColor},
    {"Audio Recorder", "Ready", kRecorderReadyTopBarColor},
    {"Audio Recorder", "Recording", kRecorderRecordingTopBarColor},
    {"Alarms", "Alarm List", kAlarmsListTopBarColor},
    {"Alarms", "Edit Alarm", kAlarmsEditTopBarColor},
    {"Morning Routine", "Quote", kMorningRoutineQuoteTopBarColor},
    {"Morning Routine", "Reflection", kMorningRoutineReflectionTopBarColor},
    {"Morning Routine", "Recording", kMorningRoutineRecordingTopBarColor},
    {"Morning Routine", "Saved", kMorningRoutineSavedTopBarColor},
    {"Morning Routine", "Error", kMorningRoutineErrorTopBarColor},
    {"Update Firmware", "Check Update", kOtaCheckTopBarColor},
    {"Update Firmware", "Progress", kOtaProgressTopBarColor},
};

}  // namespace

void beginScreenChrome(const void* screen) {
    if (s_screen == screen) return;
    s_screen = screen;
    s_hasSceneBackground = false;
    s_repaintedBar = false;
}

void setSceneChromeBackground(uint16_t color, bool repaintedBar) {
    s_sceneBackground = color;
    s_hasSceneBackground = true;
    s_repaintedBar |= repaintedBar;
}

bool takeScreenChromeRepaint() {
    const bool repaint = s_repaintedBar;
    s_repaintedBar = false;
    return repaint;
}

uint16_t screenChromeColor(const char* appName, const char* screenName) {
    if (s_hasSceneBackground) return s_sceneBackground;
    if (!appName || !screenName) return kUiBarBg;
    if (strcmp(appName, "Camera") == 0 && strcmp(screenName, "Viewfinder") == 0) return kUiBg;
    if (strcmp(appName, "WiFi Setup") == 0) return apps::wifi_setup::backgroundColor();
    for (const ScreenChromeEntry& entry : kEntries) {
        if (strcmp(entry.app, appName) == 0 && strcmp(entry.screen, screenName) == 0) {
            return entry.color;
        }
    }
    return kUiBarBg;
}
