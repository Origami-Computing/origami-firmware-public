#pragma once

#include <stdint.h>
#include "ui/layout_metrics.h"
#include "config/ui_layout.h"

namespace menu_layout {

// BEGIN apply_ui_source_edits.py menu/home
static constexpr int16_t kMenuContentBgX = 0;
static constexpr int16_t kMenuContentBgY = kSystemStatusBarH;
static constexpr int16_t kMenuContentBgW = 280;
static constexpr int16_t kMenuContentBgH = 240-kSystemStatusBarH;
static constexpr uint8_t kMenuContentBgRadius = 0;
static constexpr uint8_t kMenuContentBgZ = 0;
static constexpr int16_t kMenuListAreaX = 0;
static constexpr int16_t kMenuListAreaY = kSystemStatusBarH+2;
static constexpr int16_t kMenuListAreaW = 280;
static constexpr int16_t kMenuListAreaH = (ui::safe_area::kHeight-display_safe_area::kProfile.bottom-display_safe_area::kProfile.padding)-kMenuListAreaY;
static constexpr int16_t kMenuListRowH = ui::layout::kListRowH;
static constexpr int16_t kMenuListRowPitch = ui::layout::kListRowPitch;
static constexpr int16_t kMenuListTopPad = 0;
static constexpr int16_t kMenuListLeftX = ui::layout::kListLeftInset;
static constexpr int16_t kMenuListScrollbarW = ui::layout::kListScrollbarW;
static constexpr int16_t kMenuListScrollbarPad = ui::layout::kListScrollbarGap;
static constexpr int16_t kMenuListScrollbarRight = ui::layout::kListRightInset;// END apply_ui_source_edits.py menu/home

// Category pages align with the main menu below the status bar.
static constexpr int16_t kMenuSubListAreaY = kSystemStatusBarH+2;
static constexpr int16_t kMenuSubListAreaH = (ui::safe_area::kHeight-display_safe_area::kProfile.bottom-display_safe_area::kProfile.padding)-kMenuSubListAreaY;
static constexpr int16_t kMenuSubListTopPad = 0;

}  // namespace menu_layout
