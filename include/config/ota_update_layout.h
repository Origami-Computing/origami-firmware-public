#pragma once

#include <stdint.h>

#include "config/ui_theme.h"

namespace ota_update_layout {

// BEGIN apply_ui_source_edits.py ota/check
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

static constexpr int16_t kHeaderTitleX = 14;
static constexpr int16_t kHeaderTitleY = 40;
static constexpr int16_t kHeaderTitleW = 252;
static constexpr int16_t kHeaderTitleH = 24;
static constexpr uint8_t kHeaderTitleTextSize = 2;
static constexpr uint8_t kHeaderTitleZ = 2;
static constexpr bool kHeaderTitleVisible = true;
static constexpr uint16_t kHeaderTitleFgColor = kUiFg;
static constexpr uint16_t kHeaderTitleBgColor = kUiBg;

static constexpr int16_t kStatus0X = 18;
static constexpr int16_t kStatus0Y = 78;
static constexpr int16_t kStatus0W = 244;
static constexpr int16_t kStatus0H = 20;
static constexpr uint8_t kStatus0TextSize = 2;
static constexpr uint8_t kStatus0Z = 1;
static constexpr bool kStatus0Visible = true;
static constexpr uint16_t kStatus0BgColor = kUiBg;

static constexpr int16_t kStatus1X = 18;
static constexpr int16_t kStatus1Y = 106;
static constexpr int16_t kStatus1W = 244;
static constexpr int16_t kStatus1H = 20;
static constexpr uint8_t kStatus1TextSize = 2;
static constexpr uint8_t kStatus1Z = 1;
static constexpr bool kStatus1Visible = true;
static constexpr uint16_t kStatus1BgColor = kUiBg;

static constexpr int16_t kStatusAfterProgressX = 18;
static constexpr int16_t kStatusAfterProgressY = 156;
static constexpr int16_t kStatusAfterProgressW = 244;
static constexpr int16_t kStatusAfterProgressH = 20;
static constexpr uint8_t kStatusAfterProgressTextSize = 2;
static constexpr uint8_t kStatusAfterProgressZ = 1;
static constexpr bool kStatusAfterProgressVisible = true;
static constexpr uint16_t kStatusAfterProgressBgColor = kUiBg;

static constexpr int16_t kProgressDownloadX = 24;
static constexpr int16_t kProgressDownloadY = 132;
static constexpr int16_t kProgressDownloadW = 232;
static constexpr int16_t kProgressDownloadH = 12;
static constexpr uint8_t kProgressDownloadRadius = 3;
static constexpr uint8_t kProgressDownloadZ = 1;
static constexpr bool kProgressDownloadVisible = true;
static constexpr uint16_t kProgressDownloadTrackColor = kUiPanel;
static constexpr uint16_t kProgressDownloadFillColor = 0x03A0;

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

static constexpr int16_t kFooterHintsBgX = 0;
static constexpr int16_t kFooterHintsBgY = 210;
static constexpr int16_t kFooterHintsBgW = 280;
static constexpr int16_t kFooterHintsBgH = 30;
static constexpr uint8_t kFooterHintsBgRadius = 0;
static constexpr uint8_t kFooterHintsBgZ = 1;
static constexpr bool kFooterHintsBgVisible = true;
static constexpr uint16_t kFooterHintsBgFillColor = kUiBg;

static constexpr int16_t kFooterHint0X = 12;
static constexpr int16_t kFooterHint0Y = 212;
static constexpr int16_t kFooterHint0W = 256;
static constexpr int16_t kFooterHint0H = 12;
static constexpr uint8_t kFooterHint0TextSize = 1;
static constexpr uint8_t kFooterHint0Z = 2;
static constexpr bool kFooterHint0Visible = true;
static constexpr uint16_t kFooterHint0FgColor = kUiMuted;
static constexpr uint16_t kFooterHint0BgColor = kUiBg;

static constexpr int16_t kFooterHint1X = 12;
static constexpr int16_t kFooterHint1Y = 224;
static constexpr int16_t kFooterHint1W = 256;
static constexpr int16_t kFooterHint1H = 12;
static constexpr uint8_t kFooterHint1TextSize = 1;
static constexpr uint8_t kFooterHint1Z = 2;
static constexpr bool kFooterHint1Visible = true;
static constexpr uint16_t kFooterHint1FgColor = kUiMuted;
static constexpr uint16_t kFooterHint1BgColor = kUiBg;// END apply_ui_source_edits.py ota/check

}  // namespace ota_update_layout
