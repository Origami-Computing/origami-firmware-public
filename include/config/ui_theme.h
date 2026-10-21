#pragma once

#include <stdint.h>

/** Global light theme (RGB565). Use for page chrome; keep semantic colors for status. */

static constexpr uint16_t kUiBg     = 0xFFFF;  // white
static constexpr uint16_t kUiFg     = 0x0000;  // black
static constexpr uint16_t kUiMuted  = 0x4208;
// Soft semantic surfaces retain the light theme's black marks and readable ink.
static constexpr uint16_t kUiSuccessBg = 0xB694;  // sage green (~#B5D3A5)
static constexpr uint16_t kUiErrorBg   = 0xEC91;  // muted coral red (~#E8928A)
/** System Wi-Fi “connected” blue (~#007AFF) in RGB565 — same idea as iOS / stock Android. */
static constexpr uint16_t kUiWifiConnected = 0x03DF;
static constexpr uint16_t kUiAccent = 0x10A2;  // dark blue-grey on white
static constexpr uint16_t kUiSelBg  = 0x0000;  // black bar (selected row)
static constexpr uint16_t kUiSelFg  = 0xFFFF;  // white text on selection
static constexpr uint16_t kUiBarBg  = kUiBg;  // fallback; painted scene backgrounds take precedence

static constexpr uint16_t kUiEdit = 0x6D9F;  // light blue, active field
static constexpr uint16_t kUiScrollTrack = 0xCE79;
static constexpr uint16_t kUiScrollThumb = 0x0000;

static constexpr uint16_t kUiPanel = 0xCE79;  // light grey strip

static constexpr uint16_t kUiBatteryEmpty = 0xC618;

static constexpr uint16_t kUiShutterBlade = 0x2945;

static constexpr uint16_t kUiDialogueBorder = 0x0000;  // black
static constexpr uint16_t kUiDialogueFill   = 0xFFFF;  // white
static constexpr uint16_t kUiDialogueHint   = 0x8410;  // scroll indicator
