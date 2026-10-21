#pragma once

#include <stdint.h>

namespace agent_layout {

// BEGIN apply_ui_source_edits.py kami/recording
static constexpr int16_t kPrepareTitleCenterX = 140;
static constexpr int16_t kPrepareTitleY = 66;
static constexpr int16_t kPrepareHintCenterX = 140;
static constexpr int16_t kPrepareHintY = 102;
static constexpr int16_t kRecordingTitleCenterX = 140;
static constexpr int16_t kRecordingTitleY = 38;
static constexpr int16_t kRecordingHintCenterX = 140;
static constexpr int16_t kRecordingHintY = 70;
static constexpr int16_t kRecDotX = 260;
static constexpr int16_t kRecDotY = 220;
static constexpr int16_t kRecDotRadius = 8;
static constexpr uint16_t kRecDotIdleColor = 0x8410;
static constexpr uint16_t kRecDotActiveColor = 0xF800;
// END apply_ui_source_edits.py kami/recording

// BEGIN apply_ui_source_edits.py kami/thinking
static constexpr int16_t kThinkingCenterX = 140;
static constexpr int16_t kThinkingY = 204;
// END apply_ui_source_edits.py kami/thinking

// BEGIN apply_ui_source_edits.py kami/error
static constexpr int16_t kErrorTextX = 14;
static constexpr int16_t kErrorTextYOffset = -36;
static constexpr int16_t kErrorTextRightInset = 14;
static constexpr int16_t kErrorMaxLines = 6;
static constexpr int16_t kErrorFooterY = 218;
static constexpr int16_t kErrorTitleYOffset = -14;
static constexpr int16_t kErrorDetailYOffset = 18;
// END apply_ui_source_edits.py kami/error

}  // namespace agent_layout
