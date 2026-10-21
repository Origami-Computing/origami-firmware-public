#pragma once

#include <stdint.h>
#include "config/display_safe_area.h"

// Persistent system chrome (time + power) height; app content starts below this on canvas Y.
static constexpr int kSystemStatusBarH = display_safe_area::kStatusBarHeight;

/** Inset from left/right/top edges — rounded glass/bezel hides a few pixels in corners. */
static constexpr int kScreenInsetX = 14;
static constexpr int kScreenInsetY = 10;

// BEGIN apply_ui_source_edits.py kami/dialogue
// Mini eyes avatar (conversation mode)
static constexpr int kMiniEyeX = 26;
static constexpr int kMiniEyeY = 40;
static constexpr int kMiniEyeW = 24;
static constexpr int kMiniEyeH = 26;

// Dialogue box (conversation mode)
// Panel is 280x240 landscape. Box must stay within kScreenInsetX/Y on all
// four sides so the rounded bezel doesn't clip the corners:
//   left   = kDialogueX                            >= kScreenInsetX
//   right  = 280 - (kDialogueX + kDialogueW)       >= kScreenInsetX
//   top    = kDialogueY                            >= kSystemStatusBarH
//   bottom = 240 - (kDialogueY + kDialogueH)       >= kScreenInsetY
// Kami Dialogue: shared rails with reflection, full-height agent panel.
static constexpr int kDialogueX       = 22;
static constexpr int kDialogueY       = 78;
static constexpr int kDialogueW       = 236;
static constexpr int kDialogueH       = 150;
static constexpr int kDialogueBorderW = 6; // Total double-rule frame inset.
static constexpr int kDialogueCornerR = 4;
static constexpr int kDialoguePadX    = 10;
static constexpr int kDialoguePadY    = 4;

// Scroll-hint chevron (the grey down-arrow in the bottom-right of the dialogue
// when more reply text is below the visible window).
//   kDialogueScrollHintInsetX : px from the box's right edge to chevron x.
//   kDialogueScrollHintGapY   : px below the last text line's baseline.
// Tied to the last baseline (not the box bottom) so descenders on the final
// line don't get overdrawn by the chevron during the typewriter reveal.
static constexpr int kDialogueScrollHintInsetX = 14;
static constexpr int kDialogueScrollHintGapY   = 6;
// END apply_ui_source_edits.py kami/dialogue
