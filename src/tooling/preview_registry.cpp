#if defined(KAMI_GAMES) && KAMI_GAMES
#include "apps/tennis_app.h"
#include "apps/bowling_app.h"
#include "apps/golf_app.h"
#endif
#include "tooling/preview_registry.h"

#include <Arduino.h>
#include <string.h>

#include "apps/agent_app.h"
#include "apps/alarm_clock_app.h"
#include "apps/audio_recordings_app.h"
#include "apps/boot_app.h"
#include "apps/camera_menu_app.h"
#include "apps/doodle_jump_app.h"
#include "apps/dumb_flappy_bird_app.h"
#include "apps/hardware_test_app.h"
#include "apps/imu_calibration_app.h"
#include "apps/menu_app.h"
#include "apps/morning_routine_app.h"
#include "apps/mp3_menu_app.h"
#include "apps/ota_update_app.h"
#include "apps/pomodoro_app.h"
#include "apps/pong_app.h"
#include "apps/settings_menu_app.h"
#include "apps/steps_app.h"
#include "apps/timer_app.h"
#include "apps/tutorial_app.h"
#include "apps/tetris_app.h"
#include "apps/trex_app.h"
#include "apps/voice_recorder_app.h"
#include "apps/wifi_portal_app.h"
#include "apps/wifi_setup_app.h"
#include "scene/screen.h"

namespace tooling {
namespace preview {
namespace {

// Runtime descriptors own identity and screen membership. Hints only document controls.
struct InputBinding {
    const char* event;
    const char* action;
    const char* target;
};
struct ScreenHints {
    const char* id;
    const char* mode;
    const char* source;
    const InputBinding* inputs;
    uint8_t inputCount;
};
struct AppInfo {
    const App* runtimeApp;
    const ScreenHints* hints;
    uint8_t hintCount;
};

static bool equals(const char* left, const char* right) {
    return left && right && strcasecmp(left, right) == 0;
}

static const ::scene::Screen* runtimeScreenFor(const AppInfo& app, const char* screenId) {
    if (!screenId || !screenId[0]) return app.runtimeApp->initialScreen;
    for (uint8_t index = 0; index < app.runtimeApp->screenCount; ++index) {
        const ::scene::Screen* screen = app.runtimeApp->screens[index];
        if (screen && equals(screen->id, screenId)) return screen;
    }
    return nullptr;
}

static const InputBinding kKamiEyesInputs[] = {
    {"A tap", "open Audio Recorder", "recorder:ready"},
    {"B tap", "open Camera", "camera:viewfinder"},
    {"rotary", "open app menu", "menu:home"},
    {"C tap", "no screen action; wake/hold-to-sleep is handled globally", "kami:eyes"},
    {"A+B", "start push-to-talk recording", "kami:recording"},
};

static const InputBinding kKamiRecordingInputs[] = {
    {"A/B release", "finish and send; chord-started take waits until both buttons are up", "kami:thinking"},
    {"A/B press", "no action while recording", "kami:recording"},
    {"A+B", "consumed without action while recording", "kami:recording"},
};

static const InputBinding kKamiThinkingInputs[] = {
    {"A tap/release", "cancel request to eyes", "kami:eyes"},
    {"B press", "cancel request to menu", "menu:home"},
    {"A+B", "cancel request to menu", "menu:home"},
};

static const InputBinding kKamiReplyInputs[] = {
    {"rotary", "scroll reply text", "kami:reply"},
    {"A tap", "previous segment; from first segment return to eyes", "self-or-child"},
    {"B tap", "next segment; from last segment return to eyes", "self-or-child"},
    {"A+B", "return to eyes; continued hold can start recording", "kami:eyes"},
};

static const InputBinding kKamiErrorInputs[] = {
    {"A tap/release", "dismiss error", "kami:eyes"},
    {"B press", "open menu", "menu:home"},
    {"rotary", "dismiss error then open menu", "menu:home"},
    {"A+B", "dismiss error", "kami:eyes"},
};

static const InputBinding kMenuInputs[] = {
    {"rotary", "select app or submenu", "self"},
    {"C tap", "open selected app/submenu; Tutorial row replays tutorial", "self-or-child"},
    {"A tap / A+B", "return one menu level; at top level pop Menu", "self-or-pop"},
};

static const InputBinding kSettingsMainInputs[] = {
    {"rotary", "select row; boolean edit: toggle draft", "self"},
    {"C tap", "activate row or begin/commit boolean edit; OTA hold only on official builds", "self-or-child"},
    {"A tap / A+B", "commit boolean edit; otherwise pop Settings", "self-or-pop"},
};

static const InputBinding kSettingsWifiMenuInputs[] = {
    {"rotary", "select QR setup or saved networks", "self"},
    {"C tap", "open selected setup app or saved networks", "self-or-child"},
    {"A tap / A+B", "return to Settings main", "settings:main"},
};

static const InputBinding kSettingsSoundMenuInputs[] = {
    {"rotary", "select music output or SFX volume", "self"},
    {"C tap", "open selected sound setting", "self-or-child"},
    {"A tap / A+B", "return to Settings main", "settings:main"},
};

static const InputBinding kSettingsAgentUiInputs[] = {
    {"rotary", "browse Agent UI styles with border preview", "self"},
    {"C tap", "save highlighted style and return home", "self-or-pop"},
    {"A tap / A+B", "return without changing style", "settings:main"},
};

static const InputBinding kSettingsMusicInputs[] = {
    {"rotary", "select output route without applying it", "self"},
    {"C tap", "select and save highlighted output route", "self"},
    {"A tap / A+B", "return to Sound menu", "settings:sound-menu"},
};

static const InputBinding kSettingsVolumeInputs[] = {
    {"rotary", "adjust volume by -5 per step, clamped to 0..100; preview change", "self"},
    {"C tap / A tap / A+B", "save volume and return to Sound menu", "settings:sound-menu"},
};

static const InputBinding kSettingsWifiInputs[] = {
    {"rotary", "select saved network", "self"},
    {"C tap", "open selected network actions if selection is valid", "settings:network-action"},
    {"A tap / A+B", "return to Wi-Fi menu", "settings:wifi-menu"},
};

static const InputBinding kSettingsNetworkInputs[] = {
    {"rotary", "select Connect or Delete", "self"},
    {"C tap", "request selected action, then return to saved networks; invalid selection only returns", "settings:wifi"},
    {"A tap / A+B", "return to saved networks", "settings:wifi"},
};

static const InputBinding kSettingsUploadsInputs[] = {
    {"rotary", "select field; while selecting queued item, cycle available items", "self"},
    {"C tap", "item field: toggle selection; Send: request attempt for displayed ID; Reset: reset retry history keeping files; Sync: open full file sync", "self"},
    {"A tap / A+B", "finish item selection; otherwise return to Settings main", "self-or-child"},
};

static const InputBinding kSettingsSyncInputs[] = {
    {"C tap", "request two-way SD file sync", "self"},
    {"A tap / A+B", "return to Upload Queue", "settings:uploads"},
};

static const InputBinding kSettingsSleepInputs[] = {
    {"rotary", "select row; boolean edit: toggle enabled draft", "self"},
    {"C tap", "edit/commit enabled, open timer, save default timeout, or request battery sleep", "self-or-child"},
    {"A tap / A+B", "commit enabled draft; otherwise return to Settings main", "self-or-child"},
};

static const InputBinding kSettingsSleepTimerInputs[] = {
    {"rotary", "select minutes/seconds/Save; editing: adjust selected unit within limits", "self"},
    {"C tap", "toggle unit editing; Save: persist draft and return on success, stay on failure", "self-or-child"},
    {"B tap", "reset draft timeout to default without saving", "self"},
    {"A tap", "finish unit editing; otherwise discard draft and return to Sleep menu", "self-or-child"},
    {"A+B", "discard draft and return to Sleep menu", "settings:sleep-menu"},
};

static const InputBinding kSettingsSensorsInputs[] = {
    {"rotary", "select sensor menu row", "self"},
    {"C tap", "open Hardware Test", "self-or-child"},
    {"A tap / A+B", "return to Settings main", "settings:main"},
};


static const InputBinding kSettingsAboutInputs[] = {
    {"rotary", "scroll standard About rows and long values", "self"},
    {"C tap", "read-only information; no action", "self-or-child"},
    {"A tap / A+B", "return to Settings main", "settings:main"},
};

static const InputBinding kAlarmListInputs[] = {
    {"rotary", "select Morning Alarm, saved alarm or New Alarm", "self"},
    {"C tap", "open selected alarm editor or begin new alarm", "alarms:edit"},
    {"A tap", "unused on alarm list", "self"},
    {"A+B", "pop Alarms", "nav:pop"},
};

static const InputBinding kAlarmEditInputs[] = {
    {"rotary", "select field; editing: adjust value and preview sound/vibration", "self"},
    {"C tap", "toggle field editing; Save: attempt to save alarm", "self-or-child"},
    {"B tap", "Vibration field: toggle vibration preview; otherwise Morning editor: preview morning alarm", "self"},
    {"A tap", "finish field adjustment; otherwise cancel edit", "self-or-child"},
    {"A+B", "pop Alarms without invoking Save", "nav:pop"},
};

static const InputBinding kHardwareTestInputs[] = {
    {"A press", "if complete advance except vibration/tone; otherwise cycle colors, test A, start tone/recording or capture photo according to step", "self"},
    {"A hold/release", "vibration follows held state; release stops tone or finishes recording; vibration/tone do not advance with A", "self"},
    {"B press", "Button B step: confirm and advance; first/last step: pop; otherwise previous step", "self-or-pop"},
    {"A+B", "pop Hardware Test", "nav:pop"},
};

static const InputBinding kStepsInputs[] = {
    {"rotary", "adjust daily goal by 500 per step (1000..30000)", "steps:main"},
    {"A press", "reset today's step count", "steps:main"},
    {"A+B", "pop Steps", "nav:pop"},
};

static const InputBinding kWifiPortalInputs[] = {
    {"A press", "close portal and return", "nav:pop"},
    {"A+B", "pop Wi-Fi Portal", "nav:pop"},
};

static const InputBinding kWifiSetupInputs[] = {
    {"A tap", "normal: leave; tutorial: go back before result, ignored on result", "self-or-pop"},
    {"C tap", "normal: retry error or leave success; tutorial: unused", "self-or-pop"},
    {"B tap", "tutorial: skip or acknowledge result after 600 ms; ignored while connecting/releasing; normal: unused", "self-or-pop"},
    {"A+B", "normal: pop; tutorial: same as A tap (consumed, including on result)", "self-or-pop"},
};

// Screen-specific contracts: generic list/direct actions are not valid here.
static const InputBinding kCameraViewfinderInputs[] = {
    {"A tap", "leave Camera", "nav:pop"},
    {"B press", "capture photo using the selected capture preset", "camera:viewfinder"},
    {"C tap", "open camera settings", "camera:settings"},
    {"rotary", "no viewfinder setting change", "camera:viewfinder"},
    {"A+B", "pop Camera", "nav:pop"},
};
static const InputBinding kCameraSettingsInputs[] = {
    {"rotary", "select row; while editing adjust the draft value", "camera:settings"},
    {"C tap", "edit/commit value; Manual row or committed Manual mode opens manual controls", "self-or-child"},
    {"A tap", "cancel draft edit; otherwise return to viewfinder", "self-or-child"},
    {"A+B", "pop Camera", "nav:pop"},
};
static const InputBinding kCameraManualInputs[] = {
    {"rotary", "select row; while editing adjust the draft value", "camera:manual"},
    {"C tap", "edit/commit value; README row opens help", "self-or-child"},
    {"A tap", "cancel draft edit; otherwise return to camera settings", "self-or-child"},
    {"A+B", "pop Camera", "nav:pop"},
};
static const InputBinding kCameraReadmeInputs[] = {
    {"rotary", "scroll help text", "camera:readme"},
    {"A tap", "return to manual controls", "camera:manual"},
    {"A+B", "pop Camera", "nav:pop"},
};
static const InputBinding kCameraErrorInputs[] = {
    {"A tap", "leave Camera", "nav:pop"},
    {"A+B", "pop Camera", "nav:pop"},
};
static const InputBinding kTimerSetInputs[] = {
    {"rotary", "select minutes/seconds/sound/vibration/Start; while editing adjust value", "timer:set"},
    {"C tap", "toggle field editing; on Start begin the timer", "self-or-child"},
    {"B tap", "on Vibration toggle its preview; otherwise no action", "timer:set"},
    {"A tap", "finish editing; otherwise leave Timer", "self-or-pop"},
    {"A+B", "pop Timer", "nav:pop"},
};
static const InputBinding kTimerRunInputs[] = {
    {"C tap", "pause or resume countdown", "timer:run"},
    {"B tap", "reset to timer setup", "timer:set"},
    {"A tap", "leave Timer", "nav:pop"},
    {"A+B", "pop Timer", "nav:pop"},
};
static const InputBinding kTimerDoneInputs[] = {
    {"B/C tap or rotary", "return to timer setup", "timer:set"},
    {"A tap", "leave Timer", "nav:pop"},
    {"A+B", "pop Timer", "nav:pop"},
};
static const InputBinding kOtaCheckInputs[] = {
    {"C tap", "confirm: check; update found: download; error: retry check; otherwise no action", "self-or-child"},
    {"A tap", "request cancellation and leave OTA", "nav:pop"},
    {"A+B", "request cancellation and pop OTA", "nav:pop"},
};
static const InputBinding kOtaProgressInputs[] = {
    {"A tap", "request cancellation and leave OTA", "nav:pop"},
    {"A+B", "request cancellation and pop OTA", "nav:pop"},
};

static const InputBinding kTetrisInputs[] = {
    {"rotary", "move active piece left/right", "tetris:game"},
    {"A press", "rotate active piece; game over: restart", "tetris:game"},
    {"B press", "hard drop active piece; game over: pop Tetris", "self-or-pop"},
    {"A+B", "pop Tetris", "nav:pop"},
};

static const InputBinding kMusicBrowserInputs[] = {
    {"B tap", "open Music settings", "music:settings"},
    {"rotary", "move selected song", "music:browser"},
    {"C tap", "open selected song", "music:now-playing"},
    {"A tap", "pop Music", "nav:pop"},
    {"A+B", "pop Music", "nav:pop"},
};

static const InputBinding kMusicSettingsInputs[] = {
    {"rotary", "select setting", "music:settings"},
    {"C tap", "toggle low power or activate action", "self"},
    {"A/B tap or A+B", "return to Music", "self"},
};
static const InputBinding kMusicNowPlayingInputs[] = {
    {"rotary", "adjust volume", "music:now-playing"},
    {"A tap", "previous track", "music:now-playing"},
    {"B tap", "next track", "music:now-playing"},
    {"C tap", "play/pause", "music:now-playing"},
    {"A+B", "stop and return to song browser", "music:browser"},
};

static const InputBinding kTrexInputs[] = {
    {"B tap/press", "jump; game over: restart", "self"},
    {"A press", "duck and accelerate descent; game over: restart", "self"},
    {"A release/tap", "stop ducking; game over: A tap restarts", "self"},
    {"C tap", "pause/resume unless game over", "self"},
    {"A+B", "pop T-Rex Game", "nav:pop"},
};


static const InputBinding kPongInputs[] = {
    {"rotary", "move player paddle", "pong:game"},
    {"B tap/press", "serve or start a new match", "pong:game"},
    {"A tap/press", "reset current match", "pong:game"},
    {"C tap", "pause/resume during an active match; unused before serve or after match", "pong:game"},
    {"A+B", "pop Pong", "nav:pop"},
};


static const InputBinding kPomodoroInputs[] = {
    {"rotary", "in settings: select field or adjust its value", "pomodoro:timer"},
    {"A tap", "outside settings: switch focus/break and reset to ready", "pomodoro:timer"},
    {"B tap", "outside settings: start, pause/resume, or acknowledge completion and start next interval", "pomodoro:timer"},
    {"C tap", "open settings; in settings: edit/lock selected field", "pomodoro:timer"},
    {"A+B", "in settings: save and return; otherwise pop Pomodoro", "self-or-pop"},
};

static const InputBinding kImuCalibrationInputs[] = {
    {"rotary", "on Home with saved profile: choose calibration or visualizer", "imu-calibration:calibration"},
    {"C tap", "select, capture, advance, or save calibration", "imu-calibration:calibration"},
    {"A tap", "go back or return home", "imu-calibration:calibration"},
    {"B tap", "recenter visualizer; cancel neutral collection or retry current axis capture", "imu-calibration:calibration"},
    {"A+B", "pop IMU Calibration", "nav:pop"},
};

static const InputBinding kDoodleJumpInputs[] = {
    {"C tap", "start/restart from current pose", "doodle-jump:game"},
    {"A tap", "running: recenter; waiting/game over: start from current pose", "doodle-jump:game"},
    {"roll device", "steer character left or right", "doodle-jump:game"},
    {"A+B", "pop Doodle Jump", "nav:pop"},
};

static const InputBinding kRecordingsBrowserInputs[] = {
    {"rotary", "select browser row or deletion checkbox", "self"},
    {"C tap", "open selected recording or Delete row; deletion selection: toggle checkbox", "self-or-child"},
    {"A tap / A+B", "cancel delete confirmation/selection; otherwise pop Recordings", "self-or-pop"},
    {"B tap", "Delete row: enter selection; selection: request confirmation; confirmation: delete selected files", "self"},
};

static const InputBinding kRecordingsNowPlayingInputs[] = {
    {"rotary", "adjust volume", "recordings:now-playing"},
    {"LB tap", "stop and return to recording browser", "recordings:browser"},
    {"RB tap", "next recording", "recordings:now-playing"},
    {"RB hold", "cycle play mode", "recordings:now-playing"},
    {"C tap", "play/pause", "recordings:now-playing"},
    {"A+B", "stop and return to recording browser", "recordings:browser"},
};

static const InputBinding kRecorderInputs[] = {
    {"A tap", "recording/paused: save take and return to ready; idle: unused", "recorder:ready"},
    {"B tap", "idle: start after entry guard; recording: pause; paused: resume", "recorder:recording"},
    {"C tap", "unused", "self"},
    {"A+B", "pop Audio Recorder; app exit saves the open take", "nav:pop"},
};

static const InputBinding kMorningQuoteInputs[] = {
    {"A press", "retry unavailable quote if not already retrying", "self"},
    {"B/C press", "continue to reflection prompt", "morning-routine:reflection"},
    {"A+B", "pop Morning Routine", "nav:pop"},
};
static const InputBinding kMorningPromptInputs[] = {
    {"A press", "skip routine and return to root", "nav:root"},
    {"B/C press", "start reflection recording", "self-or-child"},
    {"A+B", "pop Morning Routine", "nav:pop"},
};
static const InputBinding kMorningRecordingInputs[] = {
    {"A press", "discard recording and return to prompt", "morning-routine:reflection"},
    {"B/C press", "save reflection", "self-or-child"},
    {"A+B", "consumed without action while recording", "self"},
};
static const InputBinding kMorningResultInputs[] = {
    {"B/C press", "finish routine and return to root", "nav:root"},
    {"A press", "unused", "self"},
    {"A+B", "pop Morning Routine", "nav:pop"},
};

static const InputBinding kBootInputs[] = {
    {"any input", "blocked during boot handoff", "boot"},
};

static const InputBinding kTutorialInputs[] = {
    {"A/B/C tap", "advance opening pages when ready; later only the indicated button advances or goes back", "self"},
    {"rotary", "complete dial lesson; navigation-practice page: open Menu", "self-or-child"},
    {"C tap", "on Wi-Fi setup page: open QR tutorial", "wifi-setup:qr-scan"},
    {"B tap", "on final page: finish tutorial and return to root", "self-or-pop"},
    {"A+B", "consumed without action throughout tutorial", "self"},
};

#define HINTS(ID, MODE, SOURCE, INPUTS) \
    {ID, MODE, SOURCE, INPUTS, \
     static_cast<uint8_t>(sizeof(INPUTS) / sizeof(INPUTS[0]))}

static const ScreenHints kKamiHints[] = {
    HINTS("eyes", "scene-mirror", "src/apps/agent/screen_eyes.cpp", kKamiEyesInputs),
    HINTS("recording", "direct-audio", "src/apps/agent/screen_recording.cpp", kKamiRecordingInputs),
    HINTS("thinking", "async-network", "src/apps/agent/screen_thinking.cpp", kKamiThinkingInputs),
    HINTS("reply", "scene-screen", "src/apps/agent/screen_reply.cpp", kKamiReplyInputs),
    HINTS("error", "scene-screen", "src/apps/agent/screen_error.cpp", kKamiErrorInputs),
};

static const ScreenHints kMenuHints[] = {
    HINTS("home", "scene-screen", "src/apps/menu_app.cpp", kMenuInputs),
};

static const ScreenHints kTutorialHints[] = {
    HINTS("learn", "scene-screen", "src/apps/tutorial_app.cpp", kTutorialInputs),
};

static const ScreenHints kBootHints[] = {
    HINTS("boot", "scene-screen", "src/apps/screen_boot.cpp", kBootInputs),
    HINTS("presents", "scene-screen", "src/apps/screen_boot.cpp", kBootInputs),
    HINTS("eyes", "scene-screen", "src/apps/screen_boot.cpp", kBootInputs),
    HINTS("handoff", "scene-screen", "src/apps/screen_boot.cpp", kBootInputs),
};

static const ScreenHints kCameraHints[] = {
    HINTS("viewfinder", "scene-screen/direct-panel", "src/apps/camera_menu_app.cpp", kCameraViewfinderInputs),
    HINTS("settings", "scene-screen", "src/apps/camera_menu_app.cpp", kCameraSettingsInputs),
    HINTS("manual", "scene-screen", "src/apps/camera_menu_app.cpp", kCameraManualInputs),
    HINTS("readme", "scene-screen", "src/apps/camera_menu_app.cpp", kCameraReadmeInputs),
    HINTS("error", "scene-screen", "src/apps/camera_menu_app.cpp", kCameraErrorInputs),
};

static const ScreenHints kSettingsHints[] = {
    HINTS("main", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsMainInputs),
    HINTS("wifi-menu", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsWifiMenuInputs),
    HINTS("sound-menu", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsSoundMenuInputs),
    HINTS("agent-style", "scene-screen/custom-preview", "src/apps/settings_menu_app.cpp", kSettingsAgentUiInputs),
    HINTS("music", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsMusicInputs),
    HINTS("sfx-volume", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsVolumeInputs),
    HINTS("wifi", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsWifiInputs),
    HINTS("network-action", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsNetworkInputs),
    HINTS("uploads", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsUploadsInputs),
    HINTS("sync", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsSyncInputs),
    HINTS("sleep-menu", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsSleepInputs),
    HINTS("sleep-timer", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsSleepTimerInputs),
    HINTS("sensors-menu", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsSensorsInputs),
    HINTS("about", "scene-screen", "src/apps/settings_menu_app.cpp", kSettingsAboutInputs),
};

static const ScreenHints kStepsHints[] = {
    HINTS("main", "scene-screen", "src/apps/steps_app.cpp", kStepsInputs),
};

static const ScreenHints kTetrisHints[] = {
    HINTS("game", "scene-screen", "src/apps/tetris_app.cpp", kTetrisInputs),
};

static const ScreenHints kTrexHints[] = {
    HINTS("game", "scene-screen", "src/apps/trex_app.cpp", kTrexInputs),
};


static const ScreenHints kPongHints[] = {
    HINTS("game", "scene-screen", "src/apps/pong_app.cpp", kPongInputs),
};

static const ScreenHints kPomodoroHints[] = {
    HINTS("timer", "scene-screen/direct-audio", "src/apps/pomodoro_app.cpp", kPomodoroInputs),
};



static const InputBinding kDumbFlappyBirdInputs[] = {
    {"yell louder", "move bird higher", "dumb-flappy-bird:game"},
    {"yell softer", "move bird lower", "dumb-flappy-bird:game"},
    {"A tap/press", "restart current run", "dumb-flappy-bird:game"},
    {"C tap", "pause/resume when microphone is ready and game is not over", "dumb-flappy-bird:game"},
    {"A+B", "pop Dumb Flappy Bird", "nav:pop"},
};

static const ScreenHints kImuCalibrationHints[] = {
    HINTS("calibration", "scene-screen/direct-panel", "src/apps/imu_calibration_app.cpp", kImuCalibrationInputs),
};

static const ScreenHints kDoodleJumpHints[] = {
    HINTS("game", "scene-screen/direct-panel", "src/apps/doodle_jump_app.cpp", kDoodleJumpInputs),
};

static const ScreenHints kDumbFlappyBirdHints[] = {
    HINTS("game", "scene-screen/direct-audio", "src/apps/dumb_flappy_bird_app.cpp", kDumbFlappyBirdInputs),
};

static const ScreenHints kHardwareHints[] = {
    HINTS("test", "scene-screen", "src/apps/hardware_test_app.cpp", kHardwareTestInputs),
};

static const ScreenHints kWifiPortalHints[] = {
    HINTS("portal", "scene-screen", "src/apps/wifi_portal_app.cpp", kWifiPortalInputs),
};

static const ScreenHints kWifiSetupHints[] = {
    HINTS("qr-scan", "scene-screen/direct-panel", "src/apps/wifi_setup_app.cpp", kWifiSetupInputs),
};

static const ScreenHints kMusicHints[] = {
    HINTS("browser", "scene-screen", "src/apps/mp3_menu_app.cpp", kMusicBrowserInputs),
    HINTS("now-playing", "scene-screen/direct-panel", "src/apps/mp3_menu_app.cpp", kMusicNowPlayingInputs),
    HINTS("settings", "scene-screen", "src/apps/mp3_menu_app.cpp", kMusicSettingsInputs),
};

static const ScreenHints kRecordingsHints[] = {
    HINTS("browser", "scene-screen", "src/apps/audio_recordings_app.cpp", kRecordingsBrowserInputs),
    HINTS("now-playing", "scene-screen/direct-panel", "src/apps/audio_recordings_app.cpp", kRecordingsNowPlayingInputs),
};

static const ScreenHints kRecorderHints[] = {
    HINTS("recording", "scene-screen/direct-panel", "src/apps/voice_recorder_app.cpp", kRecorderInputs),
    HINTS("ready", "scene-screen/direct-panel", "src/apps/voice_recorder_app.cpp", kRecorderInputs),
};

static const ScreenHints kAlarmHints[] = {
    HINTS("edit", "scene-screen", "src/apps/alarm_clock_app.cpp", kAlarmEditInputs),
    HINTS("list", "scene-screen", "src/apps/alarm_clock_app.cpp", kAlarmListInputs),
};

static const ScreenHints kMorningRoutineHints[] = {
    HINTS("quote", "scene-screen/async-network", "src/apps/morning_routine_app.cpp", kMorningQuoteInputs),
    HINTS("reflection", "scene-screen", "src/apps/morning_routine_app.cpp", kMorningPromptInputs),
    HINTS("recording", "scene-screen/direct-audio", "src/apps/morning_routine_app.cpp", kMorningRecordingInputs),
    HINTS("saved", "scene-screen", "src/apps/morning_routine_app.cpp", kMorningResultInputs),
    HINTS("error", "scene-screen", "src/apps/morning_routine_app.cpp", kMorningResultInputs),
};

static const ScreenHints kTimerHints[] = {
    HINTS("set", "scene-screen", "src/apps/timer_app.cpp", kTimerSetInputs),
    HINTS("run", "scene-screen", "src/apps/timer_app.cpp", kTimerRunInputs),
    HINTS("done", "scene-screen", "src/apps/timer_app.cpp", kTimerDoneInputs),
};

static const ScreenHints kOtaHints[] = {
    HINTS("check", "scene-screen", "src/apps/ota_update_app.cpp", kOtaCheckInputs),
    HINTS("progress", "scene-screen", "src/apps/ota_update_app.cpp", kOtaProgressInputs),
};

#undef HINTS

static const AppInfo kApps[] = {
#if defined(KAMI_GAMES) && KAMI_GAMES
    {&apps::tennis::app, nullptr, 0},
    {&apps::bowling::app, nullptr, 0},
    {&apps::golf::app, nullptr, 0},
#endif
    {&apps::tutorial::app, kTutorialHints, static_cast<uint8_t>(sizeof(kTutorialHints) / sizeof(kTutorialHints[0]))},
    {&apps::agent::app, kKamiHints, static_cast<uint8_t>(sizeof(kKamiHints) / sizeof(kKamiHints[0]))},
    {&apps::menu::app, kMenuHints, static_cast<uint8_t>(sizeof(kMenuHints) / sizeof(kMenuHints[0]))},
    {&apps::boot::app, kBootHints, static_cast<uint8_t>(sizeof(kBootHints) / sizeof(kBootHints[0]))},
    {&apps::camera_menu::app, kCameraHints, static_cast<uint8_t>(sizeof(kCameraHints) / sizeof(kCameraHints[0]))},
    {&apps::settings_menu::app, kSettingsHints, static_cast<uint8_t>(sizeof(kSettingsHints) / sizeof(kSettingsHints[0]))},
    {&apps::steps::app, kStepsHints, static_cast<uint8_t>(sizeof(kStepsHints) / sizeof(kStepsHints[0]))},
    {&apps::pomodoro::app, kPomodoroHints, static_cast<uint8_t>(sizeof(kPomodoroHints) / sizeof(kPomodoroHints[0]))},
    {&apps::imu_calibration_app::app, kImuCalibrationHints, static_cast<uint8_t>(sizeof(kImuCalibrationHints) / sizeof(kImuCalibrationHints[0]))},
    {&apps::dumb_flappy_bird::app, kDumbFlappyBirdHints, static_cast<uint8_t>(sizeof(kDumbFlappyBirdHints) / sizeof(kDumbFlappyBirdHints[0]))},
    {&apps::doodle_jump::app, kDoodleJumpHints, static_cast<uint8_t>(sizeof(kDoodleJumpHints) / sizeof(kDoodleJumpHints[0]))},
    {&apps::pong::app, kPongHints, static_cast<uint8_t>(sizeof(kPongHints) / sizeof(kPongHints[0]))},
    {&apps::tetris::app, kTetrisHints, static_cast<uint8_t>(sizeof(kTetrisHints) / sizeof(kTetrisHints[0]))},
    {&apps::trex::app, kTrexHints, static_cast<uint8_t>(sizeof(kTrexHints) / sizeof(kTrexHints[0]))},
    {&apps::hardware_test::app, kHardwareHints, static_cast<uint8_t>(sizeof(kHardwareHints) / sizeof(kHardwareHints[0]))},
    {&apps::wifi_portal::app, kWifiPortalHints, static_cast<uint8_t>(sizeof(kWifiPortalHints) / sizeof(kWifiPortalHints[0]))},
    {&apps::wifi_setup::app, kWifiSetupHints, static_cast<uint8_t>(sizeof(kWifiSetupHints) / sizeof(kWifiSetupHints[0]))},
    {&apps::mp3_menu::app, kMusicHints, static_cast<uint8_t>(sizeof(kMusicHints) / sizeof(kMusicHints[0]))},
    {&apps::audio_recordings::app, kRecordingsHints, static_cast<uint8_t>(sizeof(kRecordingsHints) / sizeof(kRecordingsHints[0]))},
    {&apps::voice_recorder::app, kRecorderHints, static_cast<uint8_t>(sizeof(kRecorderHints) / sizeof(kRecorderHints[0]))},
    {&apps::alarm_clock::app, kAlarmHints, static_cast<uint8_t>(sizeof(kAlarmHints) / sizeof(kAlarmHints[0]))},
    {&apps::morning_routine::app, kMorningRoutineHints, static_cast<uint8_t>(sizeof(kMorningRoutineHints) / sizeof(kMorningRoutineHints[0]))},
    {&apps::timer::app, kTimerHints, static_cast<uint8_t>(sizeof(kTimerHints) / sizeof(kTimerHints[0]))},
    {&apps::ota_update::app, kOtaHints, static_cast<uint8_t>(sizeof(kOtaHints) / sizeof(kOtaHints[0]))},
};

static constexpr uint8_t kAppCount = sizeof(kApps) / sizeof(kApps[0]);

const AppInfo* findApp(const char* appId) {
    if (!appId || !appId[0]) return nullptr;
    for (uint8_t index = 0; index < kAppCount; ++index) {
        if (equals(kApps[index].runtimeApp->id, appId)) return &kApps[index];
    }
    return nullptr;
}

const ScreenHints* hintsFor(const AppInfo& app, const char* screenId) {
    for (uint8_t index = 0; index < app.hintCount; ++index) {
        if (equals(app.hints[index].id, screenId)) return &app.hints[index];
    }
    return nullptr;
}

}  // namespace

void dumpApps(Print& out) {
    out.printf("[preview] apps=%u\n", kAppCount);
    for (uint8_t index = 0; index < kAppCount; ++index) {
        const AppInfo& app = kApps[index];
        out.printf("[preview] app id=%s title=\"%s\" initial=%s screens=%u source=%s\n",
                   app.runtimeApp->id,
                   app.runtimeApp->name,
                   app.runtimeApp->initialScreen->id,
                   app.runtimeApp->screenCount,
                   app.runtimeApp->source);
    }
}

bool dumpScreens(Print& out, const char* appId) {
    const AppInfo* app = findApp(appId);
    if (!app) {
        out.printf("[preview] error=\"unknown app\" app=%s\n", appId ? appId : "");
        return false;
    }
    out.printf("[preview] screens app=%s count=%u\n", app->runtimeApp->id, app->runtimeApp->screenCount);
    for (uint8_t index = 0; index < app->runtimeApp->screenCount; ++index) {
        const auto& screen = *app->runtimeApp->screens[index];
        const ScreenHints* hints = hintsFor(*app, screen.id);
        out.printf("[preview] screen app=%s id=%s title=\"%s\" mode=%s status=%s source=%s inputs=%u\n",
                   app->runtimeApp->id, screen.id, screen.name,
                   hints ? hints->mode : "unspecified",
                   hints ? "source-bound" : "inputs-undocumented",
                   hints ? hints->source : app->runtimeApp->source,
                   hints ? hints->inputCount : 0);
    }
    return true;
}

bool dumpInputs(Print& out, const char* appId, const char* screenId) {
    const AppInfo* app = findApp(appId);
    if (!app) {
        out.printf("[preview] error=\"unknown app\" app=%s\n", appId ? appId : "");
        return false;
    }
    const ::scene::Screen* screen = runtimeScreenFor(*app, screenId);
    if (!screen) {
        out.printf("[preview] error=\"unknown screen\" app=%s screen=%s\n",
                   app->runtimeApp->id, screenId ? screenId : "");
        return false;
    }
    const ScreenHints* hints = hintsFor(*app, screen->id);
    const uint8_t count = hints ? hints->inputCount : 0;
    out.printf("[preview] inputs app=%s screen=%s count=%u documented=%u\n",
               app->runtimeApp->id, screen->id, count, hints ? 1 : 0);
    for (uint8_t index = 0; index < count; ++index) {
        const InputBinding& input = hints->inputs[index];
        out.printf("[preview] input app=%s screen=%s event=\"%s\" action=\"%s\" target=%s\n",
                   app->runtimeApp->id, screen->id, input.event, input.action, input.target);
    }
    return true;
}

bool buildScene(const char* appId, const char* screenId, ui::scene::Scene& scene,
                int16_t width, int16_t height) {
    const AppInfo* app = findApp(appId);
    if (!app) return false;
    const ::scene::Screen* screen = runtimeScreenFor(*app, screenId);
    if (!screen || !screen->buildScene) return false;
    screen->buildScene(scene, width, height);
    return true;
}

}  // namespace preview
}  // namespace tooling
