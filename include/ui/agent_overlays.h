#pragma once

#include <TB_TFT_eSPI.h>
#include "apps/agent/agent_notice.h"

namespace ui {

/** White overlay: "Preparing mic / Please wait". Shown during PSRAM alloc + codec bring-up. */
void paintPttPrepare(TFT_eSPI& tft, int topY);

/** Static "Recording / Release A or B to send" label. */
void paintRecordingStatic(TFT_eSPI& tft, int topY);

/**
 * Thinking screen: clears content area, loads a single-frame eye session if
 * needed, renders the open-eye overlay, and prints "Thinking..." below.
 * Caller must have called hal::display::releaseSpiForSd() before the first SD
 * access that precedes this call.
 */
void paintThinkingScreen(TFT_eSPI& tft, int topY);

/**
 * Error screen: clears the display, renders appropriate error text, and prints
 * navigation hints at the bottom.
 *
 * @param err  Null-terminated error string (may be "HTTP NNN", "Empty reply",
 *             or any other message).
 */
void paintKamiNotice(TFT_eSPI& tft, apps::agent::notice::Kind kind, int topY);
void paintKamiError(TFT_eSPI& tft, const char* err, int topY);

}  // namespace ui
