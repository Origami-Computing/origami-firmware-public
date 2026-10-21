#pragma once

/** Boot splash BMP on SD (copy `data/origami_logo.bmp` → `/boot/origami_logo.bmp`). */
#define LOGO_BMP_PATH    "/boot/origami_logo.bmp"

#define BOOT_RGB_SWEEP_MS          3600u
#define BOOT_RGB_VALUE             255u

// BEGIN sync_screen_boot.py — generated; run: python3 tools/sync_screen_boot.py
#define LOGO_FRAME_W     280
#define LOGO_FRAME_H     120
#define LOGO_NUM_FRAMES  8
#define LOGO_PUSH_Y      57

#define LOGO_LETTER_FIRST  1
#define LOGO_LETTER_LAST     7
#define LOGO_FRAME_MS        500
#define LOGO_HOLD_MS         250

#define BOOT_PRESENTS_MS           4000u
#define BOOT_MORPH_MS              3000u
#define BOOT_HANDOFF_FLASH_IN_MS   500u
#define BOOT_HANDOFF_GROW_MS       700u
#define BOOT_HANDOFF_FADE_MS       1000u
// END sync_screen_boot.py
