#pragma once

#include <stdint.h>

// Non-secret tuning constants shared by menu firmware and apps.

/** Kami live-stream backlog. This is not a max recording length; it absorbs network/TLS jitter. */
#ifndef AGENT_STREAM_BACKLOG_SECONDS
#define AGENT_STREAM_BACKLOG_SECONDS 20
#endif
#define AGENT_MIC_RATE            48000
#define AGENT_OUT_RATE            16000
#define AGENT_DOWNSAMPLE_RATIO    (AGENT_MIC_RATE / AGENT_OUT_RATE)
#define AGENT_PCM_BUF_SIZE        (AGENT_OUT_RATE * sizeof(int16_t) * AGENT_STREAM_BACKLOG_SECONDS)

/**
 * Legacy scratch alias retained for older external callers. The WebSocket agent
 * does not allocate this buffer.
 */
#ifndef AGENT_PLAY_BUF_SIZE
#define AGENT_PLAY_BUF_SIZE 0u
#endif

/** Kami assistant reply storage (PSRAM). Per-segment cap; many `token` events append to one segment. */
#ifndef AGENT_MSG_MAX_PARTS
#define AGENT_MSG_MAX_PARTS 16
#endif
#ifndef AGENT_MSG_PART_MAX_BYTES
#define AGENT_MSG_PART_MAX_BYTES (64u * 1024u)
#endif

/**
 * Typewriter reveal for assistant text: long replies would otherwise take many minutes (one char
 * every ~44 ms). At or above INSTANT, show full window immediately; between FAST and INSTANT,
 * reveal several characters per tick and skip blips.
 */
#define AGENT_TYPEWRITER_INSTANT_CHARS   1600u
#define AGENT_TYPEWRITER_FAST_CHARS        80u
#define AGENT_TYPEWRITER_FAST_BURST_MAX    48u

#define HOME_SCREEN_BMP_PATH_PRIMARY "/eyes/eyes.bmp"
#define HOME_SCREEN_BMP_PATH_FALLBACK "/eyes/blink.bmp"

/** Optional SD-card emotion presets (frame 0 + Kami occluders). Copy JSON onto the card under /eyes/emotions/. */
#ifndef KAMI_EMOTION_JSON_THINKING
#define KAMI_EMOTION_JSON_THINKING "/eyes/emotions/thinking.json"
#endif
// BEGIN apply_ui_source_edits.py kami/eyes
static constexpr int HOME_SCREEN_FRAME_H = 204;
static constexpr int HOME_SCREEN_W = 238;
static constexpr int HOME_SCREEN_H = 204;
static constexpr int HOME_SCREEN_NUM_FRAMES = 3;
static constexpr int HOME_SCREEN_PUSH_X = 21;
static constexpr int HOME_SCREEN_PUSH_Y = 30;
static constexpr int HOME_SCREEN_BAND_OFFSET = 0;
static constexpr int HOME_SCREEN_BAND_H = 204;
// END apply_ui_source_edits.py kami/eyes

/** No A/B activity: return to Kami homescreen (unified shell) from any screen, including the app list. */
#define IDLE_RETURN_TO_KAMI_HOME_MS (60u * 1000u)
// Battery sleep timeout is persisted by core::sleep_timer (default: 60 seconds).

/** Menu list order (kApps[]) */
#define APP_INDEX_CAMERA           0
#define APP_INDEX_PHOTOS           1
#define APP_INDEX_VOICE_RECORDER   2
#define APP_INDEX_AUDIO_RECORDINGS 3
#define APP_INDEX_MP3_MENU         4
#define APP_INDEX_SETTINGS         5
#define APP_INDEX_STEPS            6
#define APP_INDEX_ALARM            7

/** Menu list order (continued) */
#define APP_INDEX_TIMER            8
#define APP_INDEX_TETRIS           9
#define APP_INDEX_WIDGETS          10

/** Nested apps — not in kApps[] */
#define APP_INDEX_KAMI             11
#define APP_INDEX_WIFI_SETUP       12
#define APP_INDEX_HARDWARE_TEST    13
