#include "services/vibration.h"
#if defined(KAMI_GAMES) && KAMI_GAMES
#include "apps/tennis_app.h"
#include "apps/bowling_app.h"
#include "apps/golf_app.h"
#endif
#include "ui/default_font_metrics.h"
#include "config/screen_chrome_layout.h"
/**
 * @file menu_app.cpp
 * @brief Hierarchical application launcher.
 */

#include "apps/menu_app.h"
#include "apps/tutorial_app.h"

#include <Arduino.h>

#include "app.h"
#include "apps/alarm_clock_app.h"
#include "apps/audio_recordings_app.h"
#include "apps/camera_menu_app.h"
#include "apps/doodle_jump_app.h"
#include "apps/dumb_flappy_bird_app.h"
#include "apps/mp3_menu_app.h"
#include "apps/ota_update_app.h"
#include "apps/photos_app.h"
#include "apps/pomodoro_app.h"
#include "apps/pong_app.h"
#include "apps/settings_menu_app.h"
#include "apps/steps_app.h"
#include "apps/timer_app.h"
#include "apps/tetris_app.h"
#include "apps/trex_app.h"
#include "apps/voice_recorder_app.h"
#include "apps/wifi_setup_app.h"
#include "config/menu_layout.h"
#include "config/ui_theme.h"
#include "assets/menu_icons.h"
#include "assets/menu_motion.h"
#include "ui/alpha_bitmap.h"
#include "hal/hal_display.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services/firmware_update.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"

namespace apps {
namespace menu {
namespace icons = assets::menu_icons;

enum Page {
  PAGE_MAIN,
  PAGE_AUDIO,
  PAGE_TIME,
  PAGE_CUSTOM_APPS,
  PAGE_GAMES,
  PAGE_PRODUCTIVITY,
  PAGE_COUNT,
};

struct MenuEntry {
  const char* label;
  App* app;
  Page child;
  const uint8_t* icon;
};

struct PageDef {
  const char* title;
  const MenuEntry* entries;
  int count;
  Page parent;
};

static const MenuEntry kMainEntries[] = {
    {"Camera", &apps::camera_menu::app, PAGE_COUNT, icons::kCamera},
    {"Photos", &apps::photos::app, PAGE_COUNT, icons::kPhotos},
    {"Audio", nullptr, PAGE_AUDIO, icons::kAudio},
    {"Music", &apps::mp3_menu::app, PAGE_COUNT, icons::kMusic},
    {"Clock", nullptr, PAGE_TIME, icons::kClock},
    {"Settings", &apps::settings_menu::app, PAGE_COUNT, icons::kSettings},
    {"Games", nullptr, PAGE_GAMES, icons::kGames},
    {"Custom Apps", nullptr, PAGE_CUSTOM_APPS, icons::kTools},
};

static const MenuEntry kAudioEntries[] = {
    {"Audio Recorder", &apps::voice_recorder::app, PAGE_COUNT, icons::kAudio},
    {"Audio Recordings", &apps::audio_recordings::app, PAGE_COUNT, icons::kRecordings},
};

static const MenuEntry kTimeEntries[] = {
    {"Alarms", &apps::alarm_clock::app, PAGE_COUNT, icons::kAlarm},
    {"Timer", &apps::timer::app, PAGE_COUNT, icons::kTimer},
    {"Pomodoro", &apps::pomodoro::app, PAGE_COUNT, icons::kFocus},
};

static const MenuEntry kCustomAppEntries[] = {
    {"Productivity", nullptr, PAGE_PRODUCTIVITY, icons::kTools},
};

static const MenuEntry kGameEntries[] = {
#if defined(KAMI_GAMES) && KAMI_GAMES
    {"Tennis", &apps::tennis::app, PAGE_COUNT, icons::kTennis},
    {"Bowling", &apps::bowling::app, PAGE_COUNT, icons::kBowling},
    {"Golf", &apps::golf::app, PAGE_COUNT, icons::kGolf},
#endif
    {"Dumb Flappy Bird", &apps::dumb_flappy_bird::app, PAGE_COUNT, icons::kBird},
    {"Doodle Jump", &apps::doodle_jump::app, PAGE_COUNT, icons::kJump},
    {"Pong", &apps::pong::app, PAGE_COUNT, icons::kPong},
    {"Tetris", &apps::tetris::app, PAGE_COUNT, icons::kBlocks},
    {"T-Rex Game", &apps::trex::app, PAGE_COUNT, icons::kDinosaur},
};

static const MenuEntry kProductivityEntries[] = {
    {"Steps", &apps::steps::app, PAGE_COUNT, icons::kSteps},
};

static const PageDef kPages[PAGE_COUNT] = {
    {nullptr, kMainEntries, (int)(sizeof(kMainEntries) / sizeof(kMainEntries[0])), PAGE_COUNT},
    {"Audio", kAudioEntries, 2, PAGE_MAIN},
    {"Clock", kTimeEntries, 3, PAGE_MAIN},
    {"Custom Apps", kCustomAppEntries, 1, PAGE_MAIN},
    {"Games", kGameEntries, (int)(sizeof(kGameEntries) / sizeof(kGameEntries[0])), PAGE_MAIN},
    {"Productivity", kProductivityEntries, (int)(sizeof(kProductivityEntries) / sizeof(kProductivityEntries[0])), PAGE_CUSTOM_APPS},
};

static Page s_page = PAGE_MAIN;
static int s_selection[PAGE_COUNT] = {};
static bool s_needRedraw = false;
static ui::ListView s_list;
static const MenuEntry* s_priorityEntry = nullptr;
static uint32_t s_motionStart = 0;
static uint8_t s_motionFrame = 0;
static bool s_iconDirty = false;

static bool animatedMainMenu() {
  return s_page == PAGE_MAIN && !apps::tutorial::navigationLessonActive();
}

static const MenuEntry kJoinWifi = {"Join Wi-Fi", &apps::wifi_setup::app, PAGE_COUNT, icons::kWifi};
static const MenuEntry kUpdateFirmware = {"Update Firmware", &apps::ota_update::app, PAGE_COUNT, nullptr};

static const PageDef& currentPage() {
  return kPages[static_cast<int>(s_page)];
}

static int visibleCount() {
  return currentPage().count + ((s_page == PAGE_MAIN && s_priorityEntry) ? 1 : 0);
}

static const MenuEntry& visibleEntry(int index) {
  if (s_page == PAGE_MAIN && s_priorityEntry) {
    if (index == 0) return *s_priorityEntry;
    --index;
  }
  return currentPage().entries[index];
}

static bool refreshPriorityRow() {
  const MenuEntry* entry = !hal::wifi::connected() ? &kJoinWifi :
      (services::firmware_update::settingsUpdateAvailable() ? &kUpdateFirmware : nullptr);
  if (entry == s_priorityEntry) return false;
  // Preserve the selected app as the row appears/disappears. Replacing Wi-Fi
  // with Update keeps the same row count and selection.
  int& selected = s_selection[PAGE_MAIN];
  if (!s_priorityEntry && entry) ++selected;
  else if (s_priorityEntry && !entry) selected = max(0, selected - 1);
  s_priorityEntry = entry;
  return true;
}

static int mainIconIndex(int index) {
  const auto mask = visibleEntry(index).icon;
  for (int i=0; i<8; ++i) if (kMainEntries[i].icon == mask) return i;
  return -1; // Priority rows do not use the animated app icons.
}

static ui::ListItem menuItemFn(int idx, void*) {
  ui::ListItem item;
  item.label = (idx >= 0 && idx < visibleCount()) ? visibleEntry(idx).label : "";
  item.icon = (idx >= 0 && idx < visibleCount()) ? visibleEntry(idx).icon : nullptr;
  item.fg = kUiFg;
  if (animatedMainMenu()) {
    item.iconSlotSize = 28;
    const int icon = mainIconIndex(idx);
    if (idx == s_selection[PAGE_MAIN] && icon >= 0) {
      using namespace assets::menu_motion;
      item.icon = kMasks[icon] + s_motionFrame*kFrameBytes;
      item.iconSize = 28;
      item.iconColor = kColors[icon];
    }
  }
  return item;
}

static void applyMenuListLayout(bool nested) {
  if (apps::tutorial::navigationLessonActive()) {
    s_list.rowH=26; s_list.rowPitch=32; s_list.topPad=4; s_list.leftX=14;
    s_list.scrollbarW=3; s_list.scrollbarPad=5; s_list.scrollbarRight=7;
    return;
  }
  s_list.rowH = menu_layout::kMenuListRowH;
  s_list.rowPitch = menu_layout::kMenuListRowPitch;
  s_list.topPad = nested ? menu_layout::kMenuSubListTopPad : menu_layout::kMenuListTopPad;
  s_list.leftX = menu_layout::kMenuListLeftX;
  s_list.scrollbarW = menu_layout::kMenuListScrollbarW;
  s_list.scrollbarPad = menu_layout::kMenuListScrollbarPad;
  s_list.scrollbarRight = menu_layout::kMenuListScrollbarRight;
}

static void redraw() {
  s_motionStart = millis();
  s_motionFrame = 0;
  s_iconDirty = false;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static bool goBackOneLevel() {
  const Page parent = currentPage().parent;
  if (parent == PAGE_COUNT) return false;
  services::vibration::navigationFeedback(services::vibration::Cue::Back);
  s_page = parent;
  redraw();
  return true;
}

void enter() {
  refreshPriorityRow();
  if (s_page == PAGE_MAIN && s_priorityEntry) s_selection[PAGE_MAIN] = 0;
  redraw();
}

static void menuTick(uint32_t now) {
  services::firmware_update::checkSettingsNotice();
  if (refreshPriorityRow()) redraw();
  if (!animatedMainMenu()) return;
  const uint8_t frame = ((now - s_motionStart) / assets::menu_motion::kFrameMs) % assets::menu_motion::kFrames;
  if (frame != s_motionFrame) {
    s_motionFrame = frame;
    s_iconDirty = true;
  }
}

void exitApp() {
  s_page = PAGE_MAIN;
}

bool onChord() {
  return goBackOneLevel();
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  int& selected = s_selection[static_cast<int>(s_page)];

  if (ev.type == ET::Scroll && ev.delta != 0) {
    s_list.totalItems = visibleCount();
    s_list.selected = selected;
    s_list.scroll(ev.delta);
    if (s_list.selected != selected) {
      selected = s_list.selected;
      apps::tutorial::noteNavigation();
      redraw();
    }
    return;
  }

  if (ui::nav::isEnterTap(ev)) {
    if (selected < 0 || selected >= visibleCount()) return;
    const MenuEntry& entry = visibleEntry(selected);
    apps::tutorial::noteNavigation();
    if (entry.app) {
      NavHandler::instance().push(entry.app);
    } else if (entry.child != PAGE_COUNT) {
      s_page = entry.child;
      redraw();
    }
    return;
  }

  if (ui::nav::isBackTap(ev) && !goBackOneLevel()) {
    NavHandler::instance().pop();
  }
}

static void buildMenuScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  const bool nested = s_page != PAGE_MAIN;
  ui::scene::Builder b(scene, width, height);

  if (nested) {
    b.clearContent(kUiBg);

  } else {
    scene.addRect("content.bg",
                  {menu_layout::kMenuContentBgX,
                   menu_layout::kMenuContentBgY,
                   menu_layout::kMenuContentBgW,
                   menu_layout::kMenuContentBgH},
                  kUiBg, menu_layout::kMenuContentBgRadius,
                  menu_layout::kMenuContentBgZ);
  }

  applyMenuListLayout(nested);
  s_list.totalItems = visibleCount();
  s_list.selected = s_selection[static_cast<int>(s_page)];
  s_list.buildScene(scene,
                    menu_layout::kMenuListAreaX,
                    nested ? menu_layout::kMenuSubListAreaY : menu_layout::kMenuListAreaY,
                    menu_layout::kMenuListAreaW,
                    (apps::tutorial::navigationLessonActive() ? 132 : (nested ? menu_layout::kMenuSubListAreaH : menu_layout::kMenuListAreaH)),
                    menuItemFn, nullptr,
                    kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);
}

void onDraw(TFT_eSprite& canvas) {
  if (!s_needRedraw) {
    if (s_iconDirty && animatedMainMenu()) {
      const int selected=s_selection[PAGE_MAIN];
      const int iconIndex=mainIconIndex(selected);
      char id[32]; snprintf(id,sizeof(id),"list.row.%d.icon",selected);
      auto* icon=ui::scene::scratchScene().find(id);
      if (iconIndex>=0 && icon) {
        using namespace assets::menu_motion;
        const auto r=icon->rect;
        ui::drawAlphaBitmap(canvas,r.x,r.y,r.w,r.h,kMasks[iconIndex]+s_motionFrame*kFrameBytes,
                            28,28,kColors[iconIndex],kUiSelBg);
        hal::display::markDirty(r.y,r.h);
      }
    }
    s_iconDirty=false;
    return;
  }
  s_needRedraw = false;
  s_iconDirty = false;
  ui::scene::Scene& scene = ui::scene::scratchScene();
  ui::scene::setCurrentScene(&scene);
  buildMenuScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
}

const ::scene::Screen kScreenMenu = {
    "Menu", enter, exitApp, menuTick, onInput, onChord, onDraw,
    "home", buildMenuScene,
    &screen_chrome_layout::kMenuHomeTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {&kScreenMenu};
App app = SCENE_APP_CATALOG("Menu", "menu", &kScreenMenu, nullptr,
                            "src/apps/menu_app.cpp", kScreens);

}  // namespace menu
}  // namespace apps
