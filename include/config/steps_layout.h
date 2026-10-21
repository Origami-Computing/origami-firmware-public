#pragma once

#include <stdint.h>

#include "config/ui_theme.h"

namespace steps_layout {

// BEGIN apply_ui_source_edits.py steps/main
static constexpr int16_t kContentBgX = 0;
static constexpr int16_t kContentBgY = 30;
static constexpr int16_t kContentBgW = 280;
static constexpr int16_t kContentBgH = 210;
static constexpr uint8_t kContentBgRadius = 0;
static constexpr uint8_t kContentBgZ = 0;
static constexpr bool kContentBgVisible = true;
static constexpr uint16_t kContentBgFillColor = kUiBg;

static constexpr int16_t kHeaderTitleBgX = 0;
static constexpr int16_t kHeaderTitleBgY = 30;
static constexpr int16_t kHeaderTitleBgW = 280;
static constexpr int16_t kHeaderTitleBgH = 22;
static constexpr uint8_t kHeaderTitleBgRadius = 0;
static constexpr uint8_t kHeaderTitleBgZ = 1;
static constexpr bool kHeaderTitleBgVisible = true;
static constexpr uint16_t kHeaderTitleBgFillColor = kUiPanel;

static constexpr int16_t kHeaderTitleX = 8;
static constexpr int16_t kHeaderTitleY = 35;
static constexpr int16_t kHeaderTitleW = 264;
static constexpr int16_t kHeaderTitleH = 8;
static constexpr uint8_t kHeaderTitleTextSize = 1;
static constexpr uint8_t kHeaderTitleZ = 2;
static constexpr bool kHeaderTitleVisible = true;
static constexpr uint16_t kHeaderTitleFgColor = kUiFg;
static constexpr uint16_t kHeaderTitleBgColor = kUiPanel;
static constexpr char kHeaderTitleText[] = "Steps";

static constexpr int16_t kStepsTotalValueX = 0;
static constexpr int16_t kStepsTotalValueY = 58;
static constexpr int16_t kStepsTotalValueW = 280;
static constexpr int16_t kStepsTotalValueH = 48;
static constexpr uint8_t kStepsTotalValueTextSize = 6;
static constexpr uint8_t kStepsTotalValueZ = 1;
static constexpr bool kStepsTotalValueVisible = true;
static constexpr uint16_t kStepsTotalValueFgColor = kUiFg;
static constexpr uint16_t kStepsTotalValueBgColor = kUiBg;

static constexpr int16_t kStepsTotalCaptionX = 0;
static constexpr int16_t kStepsTotalCaptionY = 114;
static constexpr int16_t kStepsTotalCaptionW = 280;
static constexpr int16_t kStepsTotalCaptionH = 12;
static constexpr uint8_t kStepsTotalCaptionTextSize = 1;
static constexpr uint8_t kStepsTotalCaptionZ = 1;
static constexpr bool kStepsTotalCaptionVisible = true;
static constexpr uint16_t kStepsTotalCaptionFgColor = kUiMuted;
static constexpr uint16_t kStepsTotalCaptionBgColor = kUiBg;
static constexpr char kStepsTotalCaptionText[] = "steps today";

static constexpr int16_t kStepsProgressX = 24;
static constexpr int16_t kStepsProgressY = 130;
static constexpr int16_t kStepsProgressW = 232;
static constexpr int16_t kStepsProgressH = 8;
static constexpr uint8_t kStepsProgressRadius = 3;
static constexpr uint8_t kStepsProgressZ = 1;
static constexpr bool kStepsProgressVisible = true;
static constexpr uint16_t kStepsProgressTrackColor = kUiPanel;
static constexpr uint16_t kStepsProgressFillColor = 0x03A0;

static constexpr int16_t kStepsGoalX = 0;
static constexpr int16_t kStepsGoalY = 151;
static constexpr int16_t kStepsGoalW = 280;
static constexpr int16_t kStepsGoalH = 18;
static constexpr uint8_t kStepsGoalTextSize = 1;
static constexpr uint8_t kStepsGoalZ = 1;
static constexpr bool kStepsGoalVisible = true;
static constexpr uint16_t kStepsGoalFgColor = kUiMuted;
static constexpr uint16_t kStepsGoalBgColor = kUiBg;

static constexpr int16_t kStepsYesterdayX = 0;
static constexpr int16_t kStepsYesterdayY = 169;
static constexpr int16_t kStepsYesterdayW = 280;
static constexpr int16_t kStepsYesterdayH = 18;
static constexpr uint8_t kStepsYesterdayTextSize = 1;
static constexpr uint8_t kStepsYesterdayZ = 1;
static constexpr bool kStepsYesterdayVisible = true;
static constexpr uint16_t kStepsYesterdayFgColor = kUiMuted;
static constexpr uint16_t kStepsYesterdayBgColor = kUiBg;

static constexpr int16_t kFooterBgX = 0;
static constexpr int16_t kFooterBgY = 218;
static constexpr int16_t kFooterBgW = 280;
static constexpr int16_t kFooterBgH = 22;
static constexpr uint8_t kFooterBgRadius = 0;
static constexpr uint8_t kFooterBgZ = 1;
static constexpr bool kFooterBgVisible = true;
static constexpr uint16_t kFooterBgFillColor = kUiBg;

static constexpr int16_t kFooterX = 8;
static constexpr int16_t kFooterY = 223;
static constexpr int16_t kFooterW = 264;
static constexpr int16_t kFooterH = 22;
static constexpr uint8_t kFooterTextSize = 1;
static constexpr uint8_t kFooterZ = 2;
static constexpr bool kFooterVisible = true;
static constexpr uint16_t kFooterFgColor = kUiMuted;
static constexpr uint16_t kFooterBgColor = kUiBg;
static constexpr char kFooterText[] = "A: reset    A+B: menu";// END apply_ui_source_edits.py steps/main

}  // namespace steps_layout
