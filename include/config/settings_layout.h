#pragma once

#include <stdint.h>
#include "ui/layout_metrics.h"
#include "config/ui_layout.h"

namespace settings_layout {

// BEGIN apply_ui_source_edits.py settings/main
static constexpr int16_t kSettingsContentBgX = 0;
static constexpr int16_t kSettingsContentBgY = kSystemStatusBarH;
static constexpr int16_t kSettingsContentBgW = 280;
static constexpr int16_t kSettingsContentBgH = 240-kSystemStatusBarH;
static constexpr uint8_t kSettingsContentBgRadius = 0;
static constexpr uint8_t kSettingsContentBgZ = 0;
static constexpr int16_t kSettingsListAreaX = 0;
static constexpr int16_t kSettingsListAreaY = kSystemStatusBarH+2;
static constexpr int16_t kSettingsListAreaW = 280;
static constexpr int16_t kSettingsListAreaH = 197;
static constexpr int16_t kSettingsListRowH = ui::layout::kListRowH;
static constexpr int16_t kSettingsListRowPitch = ui::layout::kListRowPitch;
static constexpr int16_t kSettingsListTopPad = 0;
static constexpr int16_t kSettingsListLeftX = ui::layout::kListLeftInset;
static constexpr int16_t kSettingsListScrollbarW = ui::layout::kListScrollbarW;
static constexpr int16_t kSettingsListScrollbarPad = ui::layout::kListScrollbarGap;
static constexpr int16_t kSettingsListScrollbarRight = ui::layout::kListRightInset;
// END apply_ui_source_edits.py settings/main

// BEGIN apply_ui_source_edits.py settings/sub-list
static constexpr int16_t kSettingsSubListAreaX = 0;
static constexpr int16_t kSettingsSubListAreaY = kSystemStatusBarH+2;
static constexpr int16_t kSettingsSubListAreaW = 280;
static constexpr int16_t kSettingsSubListAreaH = 197;
static constexpr int16_t kSettingsSubListRowH = ui::layout::kListRowH;
static constexpr int16_t kSettingsSubListRowPitch = ui::layout::kListRowPitch;
static constexpr int16_t kSettingsSubListTopPad = 0;
static constexpr int16_t kSettingsSubListLeftX = ui::layout::kListLeftInset;
static constexpr int16_t kSettingsSubListScrollbarW = ui::layout::kListScrollbarW;
static constexpr int16_t kSettingsSubListScrollbarPad = ui::layout::kListScrollbarGap;
static constexpr int16_t kSettingsSubListScrollbarRight = ui::layout::kListRightInset;
// END apply_ui_source_edits.py settings/sub-list

}  // namespace settings_layout
