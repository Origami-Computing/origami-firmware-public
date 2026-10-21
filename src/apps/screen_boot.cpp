#include "config/screen_chrome_layout.h"
/**
 * @file screen_boot.cpp
 * @brief Boot scene screens — node list built in onEnter, torn down in onExit.
 */

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "apps/boot_app.h"
#include "assets/boot_double_blink_logo.h"
#include "assets/boot_double_blink_kami.h"
#include "ui/eye_rig.h"
#include "config/boot_config.h"
#include "config/app_config.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "core/boot_animation_player.h"
#include "ui/screen_chrome.h"
#include "hal/hal_display.h"
#include "hal/hal_sd.h"
#include "nav_handler.h"
#include "scene/anim.h"
#include "scene/asset_cache.h"
#include "scene/group_node.h"
#include "scene/node.h"
#include "scene/screen.h"
#include "sfx/audio_group.h"
#include "sfx/sfx_lib.h"
#include "core/memory.h"
#include "ui/color.h"
#include "ui/home_eyes.h"
#include "ui/kami_motion.h"
#include "ui/scene.h"

namespace apps {
namespace boot {

extern const ::scene::Screen kScreenPresents;
extern const ::scene::Screen kScreenEyeMorph;
extern const ::scene::Screen kScreenHandoff;

// ─── Screen state (reset in onEnter) ─────────────────────────────────────────

struct BootState {
    uint32_t phaseStartMs = 0;
    bool     logoReady    = false;
    bool     eyesReady    = false;
    bool     bodySoundPlayed = false;
    bool     logoSoundPlayed = false;
    uint32_t signalLastFrameMs = 0;
    uint32_t signalFrameCount = 0;
    uint32_t signalRenderTotalUs = 0;
    uint32_t signalRenderMaxUs = 0;
    uint32_t signalLastDrawMs = 0;
    bool     signalFinalFrameDrawn = false;
    bool     externalAnimation = false;
    bool     timingMotionLogged = false;
    bool     timingLogoVisibleLogged = false;
    bool     timingLogoFullLogged = false;
    bool     timingMotionCompleteLogged = false;
};

static BootState s;

struct BootEyeBitmapNode {
    ::scene::NodeBase base;
    const char*       contentId = nullptr;
};

static ::scene::Group       rootGroup;
static ::scene::SpriteNode  logoSprite;
static ::scene::Group       presentsGroup;
static ::scene::LabelNode   presentsLabel;
static ::scene::LabelNode   kamiLabel;
static BootEyeBitmapNode    homeEyesSprite;
static ::scene::RectNode    flashCircle;

// Parallel audio graph (docs/scene_architecture.md — Audio section).
static ::sfx::AudioGroup audioGroup;
static sfx::SoundPlayer  soundBoot;
static sfx::SoundPlayer  soundLogoTick;
static sfx::SoundPlayer  soundPresents;
static sfx::SoundPlayer  soundEyeLand;

static constexpr const char kLogoContentBase[] = "boot/logo";
static constexpr const char kHomeEyesContentBase[] = "boot/home-eyes";
static constexpr float kBootEyesStartScale = 0.22f;
static constexpr float kBootEyesFinalScale = 0.85f;
static constexpr int kBootEyesCropTopPx = 20;
static constexpr uint32_t kBootHandoffSettleMs = 80;

// Signal Found · Clean Pull. Show a calm paper-color lead-in, then run the
// approved 3.4 s choreography at 80% speed (25% longer).
static constexpr uint32_t kSignalLeadInMs = 500u;
static constexpr uint32_t kSignalMotionDurationMs = 4250u;
static constexpr uint32_t kSignalDurationMs = kSignalLeadInMs + kSignalMotionDurationMs;
// The completed wordmark is already fully visible for the final 4% of motion
// (~170 ms), so only a short extra hold is needed before entering Kami.
static constexpr uint32_t kSignalHoldMs = 100u;
static constexpr uint32_t kSignalFrameIntervalMs = 16u;
// A validated SD animation is the authoring override; Clean Pull remains the
// zero-dependency fallback when /boot/active.oani is missing or invalid.
static constexpr bool kSignalUseSdOverride = false;
static constexpr uint16_t kSignalPaper = 0xFFDE;  // #fffaf2
static constexpr uint16_t kSignalInk   = 0x2104;  // #232323
static constexpr uint16_t kSignalRed   = 0xF965;  // #ff2c2c
static constexpr int kSignalDirtyX = 0;
static constexpr int kSignalDirtyY = 45;
static constexpr int kSignalDirtyW = 280;
static constexpr int kSignalDirtyH = 150;

static uint16_t s_eyeScaleRow[HOME_SCREEN_W];

static uint16_t bootRgbColorFor(int x, int y, uint32_t now, uint8_t val) {
    (void)x;
    (void)y;
    const uint16_t phase = (uint16_t)((now % BOOT_RGB_SWEEP_MS) * 360u / BOOT_RGB_SWEEP_MS);
    return ui::hsv565(phase, 255, val);
}

static bool isRgbTintCandidate(uint16_t px) {
    const uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
    const uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
    const uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
    const uint8_t maxC = max(r, max(g, b));
    const uint8_t minC = min(r, min(g, b));
    return maxC >= 170 && (uint8_t)(maxC - minC) <= 52;
}

static void paintBootRgbWash(TFT_eSprite& canvas, uint32_t now) {
    const int top = 0;
    canvas.fillRect(0, top, canvas.width(), canvas.height() - top,
                    bootRgbColorFor(0, 0, now, BOOT_RGB_VALUE));
    hal::display::markDirty(top, canvas.height() - top);
}

static void tintBootWhitesToRgb(TFT_eSprite& canvas, uint32_t now, int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x0 < 0) {
        w += x0;
        x0 = 0;
    }
    if (y0 < 0) {
        h += y0;
        y0 = 0;
    }
    if (x0 + w > canvas.width()) w = canvas.width() - x0;
    if (y0 + h > canvas.height()) h = canvas.height() - y0;
    if (w <= 0 || h <= 0) return;

    const uint16_t tint = bootRgbColorFor(0, 0, now, BOOT_RGB_VALUE);
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            const uint16_t px = canvas.readPixel(x, y);
            if (!isRgbTintCandidate(px)) continue;
            canvas.drawPixel(x, y, tint);
        }
    }
    hal::display::markDirty(y0, h);
}

static void tintBootNodeWhites(TFT_eSprite& canvas, uint32_t now) {
    if (logoSprite.base.visible && logoSprite.contentId && logoSprite.base.w > 0 && logoSprite.base.h > 0) {
        tintBootWhitesToRgb(canvas, now, logoSprite.base.resolvedX, logoSprite.base.resolvedY,
                            logoSprite.base.w, logoSprite.base.h);
    }
    if (presentsGroup.childCount > 0 && presentsGroup.base.w > 0 && presentsGroup.base.h > 0) {
        tintBootWhitesToRgb(canvas, now, presentsGroup.base.resolvedX, presentsGroup.base.resolvedY,
                            presentsGroup.base.w, presentsGroup.base.h);
    }
    if (homeEyesSprite.base.visible && homeEyesSprite.contentId &&
        homeEyesSprite.base.w > 0 && homeEyesSprite.base.h > 0) {
        tintBootWhitesToRgb(canvas, now, homeEyesSprite.base.resolvedX, homeEyesSprite.base.resolvedY,
                            homeEyesSprite.base.w, homeEyesSprite.base.h);
    }
}

static void bootEyeBitmapRender(::scene::NodeBase* self, TFT_eSprite& surface, uint32_t /*now*/) {
    auto* n = reinterpret_cast<BootEyeBitmapNode*>(self);
    if (!n->base.visible || !n->contentId || n->base.w <= 0 || n->base.h <= 0) return;

    int srcW = 0;
    int srcH = 0;
    const uint16_t* src = ::scene::assetPixelsForFrame(n->contentId, 0, &srcW, &srcH);
    if (!src || srcW <= 0 || srcH <= 0) return;

    const int dstX = n->base.resolvedX;
    const int dstY = n->base.resolvedY;
    const int dstW = n->base.w;
    const int dstH = n->base.h;
    if (dstW > HOME_SCREEN_W) return;

    const int cropTop = (srcH > kBootEyesCropTopPx + 8) ? kBootEyesCropTopPx : 0;
    const int croppedH = srcH - cropTop;
    surface.setSwapBytes(true);
    for (int dy = 0; dy < dstH; dy++) {
        const int sy = cropTop + (dy * croppedH) / dstH;
        for (int dx = 0; dx < dstW; dx++) {
            const int sx = (dx * srcW) / dstW;
            s_eyeScaleRow[dx] = src[(size_t)sy * (size_t)srcW + (size_t)sx];
        }
        surface.pushImage(dstX, dstY + dy, dstW, 1, s_eyeScaleRow);
    }
    surface.setSwapBytes(false);
    hal::display::markDirty(dstY, dstH);
}

static bool bootEyeBitmapIsDirty(::scene::NodeBase* self, uint32_t /*now*/) {
    return self && self->dataDirty;
}

static void bootEyeBitmapSettle(::scene::NodeBase* self, uint32_t /*now*/) {
    if (self) self->dataDirty = false;
}

static void initBootEyeBitmapNode(BootEyeBitmapNode& n) {
    memset(&n, 0, sizeof(n));
    n.base.render  = bootEyeBitmapRender;
    n.base.isDirty = bootEyeBitmapIsDirty;
    n.base.settle  = bootEyeBitmapSettle;
    n.base.visible = true;
}

static void initBootAudioNodes() {
    static bool once = false;
    if (once) return;
    sfx::initSoundPlayer(soundBoot);
    sfx::initSoundPlayer(soundLogoTick);
    sfx::initSoundPlayer(soundPresents);
    sfx::initSoundPlayer(soundEyeLand);
    once = true;
}

static void bindBootAudio() {
    initBootAudioNodes();
    audioGroup.clear();
    audioGroup.add(&soundBoot);
    audioGroup.add(&soundLogoTick);
    audioGroup.add(&soundPresents);
    audioGroup.add(&soundEyeLand);
    ::sfx::setActiveAudioGroup(&audioGroup);
}

static void releaseBootAudio() {
    audioGroup.clear();
    ::sfx::setActiveAudioGroup(nullptr);
}

// BEGIN sync_screen_boot.py — generated; run: python3 tools/sync_screen_boot.py
namespace boot_layout {

static constexpr int kLogoPushY       = 57;
static constexpr int kPresentsGroupW  = 124;
static constexpr int kPresentsGroupH  = 31;
static constexpr int kPresentsGroupX  = -75;
static constexpr int kPresentsGroupY  = 11;

static constexpr int kPresentsLabelY  = 0;
static constexpr int kPresentsLabelH  = 190;
static constexpr int kKamiLabelY      = 14;
static constexpr int kKamiLabelH      = 20;
static constexpr uint8_t kPresentsFontSize = 1;
static constexpr uint8_t kKamiFontSize     = 2;

static constexpr int kEyeStartCx  = 217;
static constexpr int kEyeStartCy  = 187;
static constexpr int kEyeEndCx    = 140;
static constexpr int kEyeEndCy    = 135;
static constexpr int kEyeBaseW    = 14;
static constexpr int kEyeBaseH    = 18;
static constexpr int kEyeBaseGap  = 6;
static constexpr int kEyeCornerR  = 4;
static constexpr float kEyeScaleMax = 3.8f;

static constexpr int kFlashW      = 40;
static constexpr int kFlashH      = 40;
static constexpr int kFlashCornerR = 20;

}  // namespace boot_layout
// END sync_screen_boot.py

// ─── Shared nodes ────────────────────────────────────────────────────────────

static void layoutScene(TFT_eSprite& canvas) {
    rootGroup.layout(canvas.width(), canvas.height());
}

static void renderScene(TFT_eSprite& canvas, uint32_t now) {
    layoutScene(canvas);
    rootGroup.render(canvas, now);
}

static void publishBootRuntimeScene() {
    ui::scene::Scene& scene = ui::scene::scratchScene();
    scene.clear();
    ui::scene::setCurrentScene(&scene);

    ui::scene::Node* logo = scene.addRect(
        "logoSprite",
        {(int16_t)logoSprite.base.resolvedX, (int16_t)logoSprite.base.resolvedY,
         (int16_t)logoSprite.base.w, (int16_t)logoSprite.base.h},
        kUiBg, 0, logoSprite.base.z);
    if (logo) logo->visible = false;

    if (presentsGroup.childCount > 0) {
        ui::scene::Node* group = scene.addRect(
            "presentsGroup",
            {(int16_t)presentsGroup.base.resolvedX, (int16_t)presentsGroup.base.resolvedY,
             (int16_t)presentsGroup.base.w, (int16_t)presentsGroup.base.h},
            kUiBg, 0, presentsGroup.base.z);
        if (group) group->visible = false;

        scene.addLabel(
            "presentsLabel",
            {(int16_t)(presentsGroup.base.resolvedX + presentsLabel.base.resolvedX),
             (int16_t)(presentsGroup.base.resolvedY + presentsLabel.base.resolvedY),
             (int16_t)presentsLabel.base.w, (int16_t)presentsLabel.base.h},
            presentsLabel.text, presentsLabel.color, kUiBg, presentsLabel.textSize,
            ui::scene::TextAlign::TopCenter, presentsLabel.base.z + 1);
        scene.addLabel(
            "kamiLabel",
            {(int16_t)(presentsGroup.base.resolvedX + kamiLabel.base.resolvedX),
             (int16_t)(presentsGroup.base.resolvedY + kamiLabel.base.resolvedY),
             (int16_t)kamiLabel.base.w, (int16_t)kamiLabel.base.h},
            kamiLabel.text, kamiLabel.color, kUiBg, kamiLabel.textSize,
            ui::scene::TextAlign::TopCenter, kamiLabel.base.z + 1);
    }
    if (homeEyesSprite.base.visible && homeEyesSprite.base.w > 0 && homeEyesSprite.base.h > 0) {
        scene.addRect(
            "homeEyesSprite",
            {(int16_t)homeEyesSprite.base.resolvedX, (int16_t)homeEyesSprite.base.resolvedY,
             (int16_t)homeEyesSprite.base.w, (int16_t)homeEyesSprite.base.h},
            kUiFg, 0, homeEyesSprite.base.z);
    }

    if (flashCircle.base.visible) {
        scene.addRect(
            "flashCircle",
            {(int16_t)flashCircle.base.resolvedX, (int16_t)flashCircle.base.resolvedY,
             (int16_t)flashCircle.base.w, (int16_t)flashCircle.base.h},
            flashCircle.fillColor, flashCircle.cornerR, flashCircle.base.z);
    }
}

static void addBootLogoPreview(ui::scene::Scene& scene, int16_t width) {
    scene.addRect("logoSprite", {0, boot_layout::kLogoPushY, width, LOGO_FRAME_H},
                  kUiBg, 0, 1);
    scene.addLabel("logo.label", {0, (int16_t)(boot_layout::kLogoPushY + 42), width, 24},
                   "ORIGAMI", kUiFg, kUiBg, 3,
                   ui::scene::TextAlign::TopCenter, 2);
}

static void buildBootLogoScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("boot.bg", {0, kSystemStatusBarH, width,
                              (int16_t)(height - kSystemStatusBarH)},
                  kUiBg, 0, 0);
    addBootLogoPreview(scene, width);
}

static void buildBootPresentsScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    buildBootLogoScene(scene, width, height);
    const int groupX = (width - boot_layout::kPresentsGroupW) / 2;
    const int groupY = boot_layout::kLogoPushY + LOGO_FRAME_H + boot_layout::kPresentsGroupY;
    scene.addRect("presentsGroup",
                  {(int16_t)groupX, (int16_t)groupY,
                   boot_layout::kPresentsGroupW, boot_layout::kPresentsGroupH},
                  kUiBg, 0, 2);
    scene.addLabel("presentsLabel",
                   {(int16_t)groupX, (int16_t)(groupY + boot_layout::kPresentsLabelY),
                    boot_layout::kPresentsGroupW, boot_layout::kPresentsLabelH},
                   "presents", kUiMuted, kUiBg, boot_layout::kPresentsFontSize,
                   ui::scene::TextAlign::TopCenter, 3);
    scene.addLabel("kamiLabel",
                   {(int16_t)groupX, (int16_t)(groupY + boot_layout::kKamiLabelY),
                    boot_layout::kPresentsGroupW, boot_layout::kKamiLabelH},
                   "Kami", kUiFg, kUiBg, boot_layout::kKamiFontSize,
                   ui::scene::TextAlign::TopCenter, 3);
}

static void buildBootEyesScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("boot.bg", {0, kSystemStatusBarH, width,
                              (int16_t)(height - kSystemStatusBarH)},
                  kUiBg, 0, 0);
    scene.addRect("homeEyesSprite",
                  {HOME_SCREEN_PUSH_X, HOME_SCREEN_PUSH_Y,
                   HOME_SCREEN_W, HOME_SCREEN_FRAME_H},
                  kUiFg, 0, 4);
}

static void buildBootHandoffScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    buildBootEyesScene(scene, width, height);
}

/** Clear content band, repaint entire scene tree (root partial-dirty skips after erase). */
static void bootDrawContent(TFT_eSprite& canvas) {
    const uint32_t now = millis();
    paintBootRgbWash(canvas, now);
    ::scene::markGroupDirty(rootGroup);
    renderScene(canvas, now);
    tintBootNodeWhites(canvas, now);
    publishBootRuntimeScene();
}

static void clearScene() {
    rootGroup.clear();
    for (int i = 0; i < LOGO_NUM_FRAMES; i++) {
        char id[24];
        snprintf(id, sizeof(id), "%s:%d", kLogoContentBase, i);
        ::scene::assetRelease(id);
    }
    ::scene::assetRelease("boot/home-eyes:0");
    ::scene::assetCacheClear();
}

static void preloadLogoFrames() {
    if (!hal::sd::mounted()) {
        s.logoReady = false;
        Serial.println("[boot] logo skipped: SD unavailable");
        return;
    }
    hal::display::releaseSpiForSd();
    s.logoReady = hal::sd::exists(LOGO_BMP_PATH);
    if (!s.logoReady) {
        Serial.printf("[boot] missing BMP on SD: %s\n", LOGO_BMP_PATH);
        return;
    }
    for (int i = 0; i < LOGO_NUM_FRAMES; i++) {
        char id[24];
        snprintf(id, sizeof(id), "%s:%d", kLogoContentBase, i);
        const int32_t row = (int32_t)i * LOGO_FRAME_H;
        if (!::scene::assetPreload(id, LOGO_BMP_PATH, LOGO_FRAME_W, LOGO_FRAME_H, row,
                                  ::scene::AssetPriority::High)) {
            Serial.printf("[boot] preload failed frame %d (%s)\n", i, id);
            s.logoReady = false;
            return;
        }
    }
    Serial.println("[boot] logo frames preloaded");
}

static void preloadHomeEyesFrame() {
    hal::display::releaseSpiForSd();
    s.eyesReady = ::scene::assetPreload("boot/home-eyes:0", HOME_SCREEN_BMP_PATH_PRIMARY,
                                        HOME_SCREEN_W, HOME_SCREEN_FRAME_H, 0,
                                        ::scene::AssetPriority::High);
    if (!s.eyesReady) {
        Serial.printf("[boot] missing/preload failed eyes BMP: %s\n", HOME_SCREEN_BMP_PATH_PRIMARY);
    } else {
        Serial.println("[boot] home eyes frame preloaded");
    }
}

static void buildLogoScene() {
    initGroupNode(rootGroup, true);
    initSpriteNode(logoSprite);

    logoSprite.base.z        = 0;
    logoSprite.base.alignX   = ::scene::Align::Fixed;
    logoSprite.base.alignY   = ::scene::Align::Fixed;
    logoSprite.base.x        = 0;
    logoSprite.base.y        = boot_layout::kLogoPushY;
    logoSprite.base.w        = LOGO_FRAME_W;
    logoSprite.base.h        = LOGO_FRAME_H;
    logoSprite.contentId     = s.logoReady ? kLogoContentBase : nullptr;
    logoSprite.opacity.snap(1.f);
    logoSprite.frameAnim.snap(LOGO_LETTER_LAST);

    rootGroup.clear();
    rootGroup.add(&logoSprite.base);
}

static void buildPresentsNodes() {
    initGroupNode(presentsGroup, false);
    initLabelNode(presentsLabel);
    initLabelNode(kamiLabel);

    presentsGroup.base.z      = 1;
    presentsGroup.base.alignX = ::scene::Align::Center;
    presentsGroup.base.alignY = ::scene::Align::PinFar;
    presentsGroup.base.x      = boot_layout::kPresentsGroupX;
    presentsGroup.base.y      = boot_layout::kPresentsGroupY;
    presentsGroup.base.w      = boot_layout::kPresentsGroupW;
    presentsGroup.base.h      = boot_layout::kPresentsGroupH;
    presentsGroup.bgColor     = kUiBg;

    presentsLabel.base.z       = 0;
    presentsLabel.base.alignX  = ::scene::Align::Center;
    presentsLabel.base.alignY  = ::scene::Align::Fixed;
    presentsLabel.base.x       = 0;
    presentsLabel.base.y       = boot_layout::kPresentsLabelY;
    presentsLabel.base.w       = boot_layout::kPresentsGroupW;
    presentsLabel.base.h       = boot_layout::kPresentsLabelH;
    strncpy(presentsLabel.text, "presents", sizeof(presentsLabel.text) - 1);
    presentsLabel.fontId   = 1;
    presentsLabel.textSize = boot_layout::kPresentsFontSize;
    presentsLabel.color    = kUiMuted;

    kamiLabel.base.z       = 0;
    kamiLabel.base.alignX  = ::scene::Align::Center;
    kamiLabel.base.alignY  = ::scene::Align::Fixed;
    kamiLabel.base.x       = 0;
    kamiLabel.base.y       = boot_layout::kKamiLabelY;
    kamiLabel.base.w       = boot_layout::kPresentsGroupW;
    kamiLabel.base.h       = boot_layout::kKamiLabelH;
    strncpy(kamiLabel.text, "Kami", sizeof(kamiLabel.text) - 1);
    kamiLabel.fontId   = 1;
    kamiLabel.textSize = boot_layout::kKamiFontSize;
    kamiLabel.color    = kUiFg;

    presentsGroup.clear();
    presentsGroup.add(&presentsLabel.base);
    presentsGroup.add(&kamiLabel.base);

    TFT_eSPI& tft = hal::display::tft();
    presentsGroup.sprite =
        core::memory::allocSprite("boot.presents", tft, presentsGroup.base.w, presentsGroup.base.h);
}

static void placeHomeEyesForProgress(float u) {
    if (u < 0.f) u = 0.f;
    if (u > 1.f) u = 1.f;
    const float ease = u;
    const float scale = kBootEyesStartScale + (kBootEyesFinalScale - kBootEyesStartScale) * ease;
    const int w = (int)((float)HOME_SCREEN_W * scale);
    const int h = (int)((float)(HOME_SCREEN_FRAME_H - kBootEyesCropTopPx) * scale);
    const int startW = (int)((float)HOME_SCREEN_W * kBootEyesStartScale);
    const int startH = (int)((float)(HOME_SCREEN_FRAME_H - kBootEyesCropTopPx) * kBootEyesStartScale);
    const int startX = 280 - kScreenInsetX - startW;
    const int startY = 240 - kScreenInsetY - startH;
    const int endW = (int)((float)HOME_SCREEN_W * kBootEyesFinalScale);
    const int endFullH = (int)((float)HOME_SCREEN_FRAME_H * kBootEyesFinalScale);
    const int endX = (280 - endW) / 2;
    const int endY = kSystemStatusBarH + ((240 - kSystemStatusBarH - endFullH) / 2);

    homeEyesSprite.base.x = startX + (int)((endX - startX) * ease);
    homeEyesSprite.base.y = startY + (int)((endY - startY) * ease);
    homeEyesSprite.base.w = w;
    homeEyesSprite.base.h = h;
    homeEyesSprite.base.markDirty();
}

static void buildEyeMorphNodes() {
    initBootEyeBitmapNode(homeEyesSprite);
    homeEyesSprite.base.z      = 2;
    homeEyesSprite.base.alignX = ::scene::Align::Fixed;
    homeEyesSprite.base.alignY = ::scene::Align::Fixed;
    homeEyesSprite.contentId   = s.eyesReady ? kHomeEyesContentBase : nullptr;
    placeHomeEyesForProgress(0.f);
}

// ─── Signal Found · Clean Pull ──────────────────────────────────────────────

static float signalClamp(float value) {
    if (value < 0.f) return 0.f;
    if (value > 1.f) return 1.f;
    return value;
}

static float signalPhase(float progress, float start, float end) {
    return signalClamp((progress - start) / (end - start));
}

static float signalLerp(float from, float to, float progress) {
    return from + (to - from) * progress;
}

static float signalEaseInOut(float value) {
    const float t = signalClamp(value);
    if (t < 0.5f) return 4.f * t * t * t;
    const float u = -2.f * t + 2.f;
    return 1.f - (u * u * u) * 0.5f;
}

static float signalEaseIn(float value) {
    const float t = signalClamp(value);
    return t * t * t;
}

static float signalEaseOutBack(float value) {
    const float t = signalClamp(value) - 1.f;
    return 1.f + 2.2f * t * t * t + 1.2f * t * t;
}

static float signalSettle(float value) {
    const float t = signalClamp(value);
    return 1.f + expf(-6.5f * t) * sinf(14.f * t) * 0.055f;
}

static float signalBlinkScale(float progress, float start, float end) {
    if (progress <= start || progress >= end) return 1.f;
    const float t = signalPhase(progress, start, end);
    return 0.07f + 0.93f * fabsf(cosf(PI * t));
}

static uint8_t signalAlpha4At(const uint8_t* packed, int width, int x, int y) {
    const size_t pixelIndex = (size_t)y * (size_t)width + (size_t)x;
    const uint8_t pair = pgm_read_byte(&packed[pixelIndex >> 1]);
    return (pixelIndex & 1u) ? (pair & 0x0Fu) : (pair >> 4);
}

// Bilinearly scale a packed soft-alpha mask and composite it over the pixels
// already in the framebuffer. This preserves the HTML layer order: body over
// paper, then eyes over the partially transparent body during the handoff.
static void drawSignalMask(TFT_eSprite& canvas, const uint8_t* packed,
                           int srcW, int srcH, int centerX, int centerY,
                           int dstW, int dstH, uint16_t color, uint8_t opacity,
                           int clipWidth = -1) {
    if (!packed || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || opacity == 0) return;
    if (dstW > HOME_SCREEN_W) return;
    if (clipWidth < 0 || clipWidth > dstW) clipWidth = dstW;

    const int left = centerX - dstW / 2;
    const int top = centerY - dstH / 2;
    const int drawLeft = max(0, left);
    const int drawRight = min((int)canvas.width(), left + clipWidth);
    const int drawTop = max(0, top);
    const int drawBottom = min((int)canvas.height(), top + dstH);
    if (drawLeft >= drawRight || drawTop >= drawBottom) return;

    const int srcMaxX = srcW - 1;
    const int srcMaxY = srcH - 1;
    uint16_t* framebuffer = (uint16_t*)canvas.getPointer();
    if (!framebuffer) return;
    const int stride = canvas.width();
    for (int py = drawTop; py < drawBottom; ++py) {
        const int dy = py - top;
        const uint32_t sy256 = dstH > 1
            ? (uint32_t)((uint64_t)dy * (uint64_t)srcMaxY * 256u / (uint32_t)(dstH - 1))
            : 0u;
        const int sy0 = min(srcMaxY, (int)(sy256 >> 8));
        const int sy1 = min(srcMaxY, sy0 + 1);
        const uint16_t fy = (uint16_t)(sy256 & 0xFFu);

        for (int px = drawLeft; px < drawRight; ++px) {
            const int dx = px - left;
            const uint32_t sx256 = dstW > 1
                ? (uint32_t)((uint64_t)dx * (uint64_t)srcMaxX * 256u / (uint32_t)(dstW - 1))
                : 0u;
            const int sx0 = min(srcMaxX, (int)(sx256 >> 8));
            const int sx1 = min(srcMaxX, sx0 + 1);
            const uint16_t fx = (uint16_t)(sx256 & 0xFFu);

            const uint16_t a00 = signalAlpha4At(packed, srcW, sx0, sy0);
            const uint16_t a10 = signalAlpha4At(packed, srcW, sx1, sy0);
            const uint16_t a01 = signalAlpha4At(packed, srcW, sx0, sy1);
            const uint16_t a11 = signalAlpha4At(packed, srcW, sx1, sy1);
            const uint32_t topAlpha = a00 * (256u - fx) + a10 * fx;
            const uint32_t bottomAlpha = a01 * (256u - fx) + a11 * fx;
            const uint8_t level = (uint8_t)((topAlpha * (256u - fy) + bottomAlpha * fy + 32768u) >> 16);
            const uint8_t alpha = (uint8_t)(((uint16_t)level * 17u * opacity + 127u) / 255u);
            if (alpha == 0) continue;
            uint16_t& raw = framebuffer[(size_t)py * (size_t)stride + (size_t)px];
            const uint16_t background = (uint16_t)((raw >> 8) | (raw << 8));
            const uint16_t blended = alpha == 255
                ? color : canvas.alphaBlend(alpha, color, background);
            raw = (uint16_t)((blended >> 8) | (blended << 8));
        }
    }
}

static void drawSignalBody(TFT_eSprite& canvas, float timelineProgress,
                           float growthProgress, uint8_t opacity) {
    if (growthProgress <= 0.f || opacity == 0) return;
    const float scaleX = signalLerp(0.20f, 1.f, growthProgress);
    const float scaleY = signalLerp(0.06f, 1.f, growthProgress) *
                         signalSettle(signalPhase(timelineProgress, 0.54f, 0.67f));
    drawSignalMask(canvas, assets::kBootKamiBodyAlpha4,
                   assets::kBootKamiBodyW, assets::kBootKamiBodyH,
                   140, 117,
                   max(1, (int)lroundf((float)assets::kBootKamiBodyW * scaleX)),
                   max(1, (int)lroundf((float)assets::kBootKamiBodyH * scaleY)),
                   kSignalRed, opacity);
}

static void drawSignalEyes(TFT_eSprite& canvas, float scaleY, uint8_t opacity) {
    ui::eye_rig::draw(canvas, 140-assets::kBootKamiEyesW/2,
                     116-assets::kBootKamiEyesH/2,
                     assets::kBootKamiEyesW, assets::kBootKamiEyesH,
                     ui::eye_rig::kPresets[0].pose, scaleY, kSignalPaper,
                     0, 0, 0, true, opacity);
}

static void drawSignalRigidKami(TFT_eSprite& canvas, int centerX, int centerY,
                                int width, int eyeShiftX, float eyeScaleY,
                                uint8_t opacity) {
    if (width <= 0 || opacity == 0) return;
    const float scale = (float)width / (float)assets::kBootKamiBodyW;
    const int bodyHeight = max(1, (int)lroundf((float)assets::kBootKamiBodyH * scale));
    const int eyeWidth = max(1, (int)lroundf((float)assets::kBootKamiEyesW * scale));
    const int eyeHeight = max(1, (int)lroundf(
        (float)assets::kBootKamiEyesH * scale));
    drawSignalMask(canvas, assets::kBootKamiBodyAlpha4,
                   assets::kBootKamiBodyW, assets::kBootKamiBodyH,
                   centerX, centerY, width, bodyHeight, kSignalRed, opacity);
    ui::eye_rig::draw(canvas, centerX+eyeShiftX-eyeWidth/2,
                     centerY-1-eyeHeight/2, eyeWidth, eyeHeight,
                     ui::eye_rig::kPresets[0].pose, eyeScaleY, kSignalPaper,
                     0, 0, 0, true, opacity);
}

static void drawSignalCircle(TFT_eSprite& canvas, int centerX, int centerY,
                             int radius, uint16_t color, uint8_t opacity) {
    if (radius <= 0 || opacity == 0) return;
    if (opacity == 255) {
        canvas.fillCircle(centerX, centerY, radius, color);
        return;
    }
    uint16_t* framebuffer = (uint16_t*)canvas.getPointer();
    if (!framebuffer) return;
    const int stride = canvas.width();
    const int radiusSquared = radius * radius;
    for (int y = max(0, centerY - radius); y <= min((int)canvas.height() - 1, centerY + radius); ++y) {
        for (int x = max(0, centerX - radius); x <= min((int)canvas.width() - 1, centerX + radius); ++x) {
            const int dx = x - centerX;
            const int dy = y - centerY;
            if (dx * dx + dy * dy > radiusSquared) continue;
            uint16_t& raw = framebuffer[(size_t)y * (size_t)stride + (size_t)x];
            const uint16_t background = (uint16_t)((raw >> 8) | (raw << 8));
            const uint16_t blended = canvas.alphaBlend(opacity, color, background);
            raw = (uint16_t)((blended >> 8) | (blended << 8));
        }
    }
}

static void drawSignalLogo(TFT_eSprite& canvas, float reveal, float scale,
                           uint8_t opacity = 255) {
    const int width = max(1, (int)lroundf((float)assets::kBootDoubleBlinkLogoW * scale));
    const int height = max(1, (int)lroundf((float)assets::kBootDoubleBlinkLogoH * scale));
    const int clipWidth = max(0, min(width, (int)lroundf((float)width * reveal)));
    drawSignalMask(canvas, assets::kBootDoubleBlinkLogoAlpha4,
                   assets::kBootDoubleBlinkLogoW, assets::kBootDoubleBlinkLogoH,
                   canvas.width() / 2, canvas.height() / 2,
                   width, height, kSignalInk, opacity, clipWidth);
}

static void drawSignalPaperTexture(TFT_eSprite& canvas, int x0, int y0, int x1, int y1) {
    // Equivalent of the preview's 0.028-alpha, 0.35 px paper speckles after
    // 4x downsampling. Blend against the pixel already in the framebuffer,
    // matching Canvas2D source-over behavior. Pre-blending against paper and
    // then writing that color opaquely created cream pixels over Kami and the
    // wordmark at the texture's regular 23 x 13 px spacing.
    uint16_t* framebuffer = (uint16_t*)canvas.getPointer();
    if (!framebuffer) return;
    const int stride = canvas.width();
    for (int y = 7; y < canvas.height(); y += 13) {
        if (y < y0 || y >= y1) continue;
        for (int x = (y * 5) % 23; x < canvas.width(); x += 23) {
            if (x < x0 || x >= x1) continue;
            uint16_t& raw = framebuffer[(size_t)y * (size_t)stride + (size_t)x];
            const uint16_t background = (uint16_t)((raw >> 8) | (raw << 8));
            const uint16_t blended = canvas.alphaBlend(2, kSignalInk, background);
            raw = (uint16_t)((blended >> 8) | (blended << 8));
        }
    }
}

static void drawSignalBoot(TFT_eSprite& canvas, float progress) {
    // This animation paints directly instead of rendering its editor scene.
    setSceneChromeBackground(kSignalPaper, s.signalFrameCount == 0);
    const bool firstFrame = s.signalFrameCount == 0;
    if (firstFrame) canvas.fillSprite(kSignalPaper);
    else canvas.fillRect(kSignalDirtyX, kSignalDirtyY, kSignalDirtyW, kSignalDirtyH, kSignalPaper);

    const float eyesIn = signalEaseOutBack(signalPhase(progress, 0.02f, 0.10f));
    const float bodyGrow = signalEaseOutBack(signalPhase(progress, 0.38f, 0.58f));
    const float eyeScale = min(
        signalBlinkScale(progress, 0.12f, 0.21f),
        min(signalBlinkScale(progress, 0.26f, 0.35f),
            signalBlinkScale(progress, 0.61f, 0.69f)));

    if (progress < 0.68f) {
        drawSignalBody(canvas, progress, bodyGrow, 255);
        drawSignalEyes(canvas, eyeScale,
                       (uint8_t)lroundf(255.f * signalClamp(eyesIn)));
    } else if (progress < 0.76f) {
        const float ready = signalEaseInOut(signalPhase(progress, 0.68f, 0.76f));
        const int kamiX = (int)lroundf(signalLerp(140.f, 86.f, ready));
        const int kamiWidth = (int)lroundf(signalLerp(106.f, 92.f, ready));
        const int eyeShift = (int)lroundf(signalLerp(0.f, -3.f, ready));
        drawSignalRigidKami(canvas, kamiX, 117, kamiWidth, eyeShift,
                            eyeScale, 255);
    } else {
        const float pull = signalEaseInOut(signalPhase(progress, 0.76f, 0.96f));
        const float edge = signalLerp(30.f, 250.f, pull);
        const float exit = signalEaseIn(signalPhase(pull, 0.70f, 0.96f));
        const uint8_t kamiOpacity = (uint8_t)lroundf(255.f * (1.f - exit));
        const float logoScale = signalLerp(0.985f, 1.f, signalSettle(pull));

        // The wordmark is clipped at the hand position, so it reads as an
        // object Kami is pulling rather than an unrelated wipe transition.
        drawSignalLogo(canvas, pull, logoScale);
        if (kamiOpacity > 0) {
            const int handX = (int)lroundf(edge) + 1;
            drawSignalCircle(canvas, handX, 107, 9, kSignalRed, kamiOpacity);
            drawSignalCircle(canvas, handX, 127, 9, kSignalRed, kamiOpacity);
            drawSignalRigidKami(canvas, (int)lroundf(edge) + 56, 117,
                                92, -3, 1.f, kamiOpacity);
        }
    }

    drawSignalPaperTexture(canvas,
                           firstFrame ? 0 : kSignalDirtyX,
                           firstFrame ? 0 : kSignalDirtyY,
                           firstFrame ? canvas.width() : kSignalDirtyX + kSignalDirtyW,
                           firstFrame ? canvas.height() : kSignalDirtyY + kSignalDirtyH);
    if (firstFrame) hal::display::markAllDirty();
    else hal::display::markDirty(kSignalDirtyY, kSignalDirtyH);
}

static void signalBootOnEnter() {
    s = BootState{};
    s.phaseStartMs = millis();
    s.signalLastFrameMs = s.phaseStartMs;
    s.externalAnimation = kSignalUseSdOverride && core::boot_animation::begin();
    bindBootAudio();
    soundBoot.playFromDef(sfx::kSfxBoot, s.phaseStartMs);
    hal::display::markAllDirty();
    if (s.externalAnimation) {
        Serial.println("[boot] SD animation selected; embedded clean-pull is fallback");
    } else {
        Serial.println("[boot] Signal Found clean-pull embedded sequence");
    }
    Serial.printf("[boot-timing] event=phase_start millis=%lu lead_ms=%lu motion_ms=%lu hold_ms=%lu\n",
                  (unsigned long)s.phaseStartMs,
                  (unsigned long)kSignalLeadInMs,
                  (unsigned long)kSignalMotionDurationMs,
                  (unsigned long)kSignalHoldMs);
}

static void signalBootOnExit() {
    core::boot_animation::end();
    releaseBootAudio();
}

static void signalBootOnTick(uint32_t now) {
    const uint32_t elapsed = now - s.phaseStartMs;
    const uint32_t duration = s.externalAnimation
        ? core::boot_animation::durationMs() : kSignalDurationMs;
    const uint32_t hold = s.externalAnimation
        ? core::boot_animation::holdMs() : kSignalHoldMs;
    const uint32_t motionStart = s.externalAnimation ? 0u : kSignalLeadInMs;
    const uint32_t motionDuration = s.externalAnimation
        ? duration : kSignalMotionDurationMs;
    if (!s.bodySoundPlayed && elapsed >= motionStart + (uint32_t)(motionDuration * 0.38f)) {
        s.bodySoundPlayed = true;
        soundEyeLand.playFromDef(sfx::kSfxBootEyeLand, now);
    }
    if (!s.logoSoundPlayed && elapsed >= motionStart + (uint32_t)(motionDuration * 0.76f)) {
        s.logoSoundPlayed = true;
        soundLogoTick.playFromDef(sfx::kSfxBootLogoTick, now);
    }
    if (elapsed >= duration + hold) {
        const uint32_t averageRenderUs = s.signalFrameCount
            ? s.signalRenderTotalUs / s.signalFrameCount : 0u;
        const uint32_t frames = s.externalAnimation
            ? core::boot_animation::renderedFrameCount() : s.signalFrameCount;
        Serial.printf("[boot] animation complete source=%s -> Kami frames=%lu active_fps=%lu render_avg_us=%lu render_max_us=%lu\n",
                      s.externalAnimation ? "sd-oani" : "embedded",
                      (unsigned long)frames,
                      (unsigned long)(frames * 1000u / duration),
                      (unsigned long)averageRenderUs,
                      (unsigned long)s.signalRenderMaxUs);
        completeAnimation(now, s.phaseStartMs, duration + hold);
        return;
    }
    if (!s.externalAnimation && now - s.signalLastFrameMs >= kSignalFrameIntervalMs) {
        s.signalLastFrameMs = now;
        hal::display::markDirty(kSignalDirtyY, kSignalDirtyH);
    }
}

static void signalBootOnDraw(TFT_eSprite& canvas) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - s.phaseStartMs;
    if (s.externalAnimation) {
        if (core::boot_animation::renderDue(canvas, elapsed)) return;
        Serial.printf("[boot] external animation failed at %lu ms; using embedded fallback\n",
                      (unsigned long)elapsed);
        s.externalAnimation = false;
        s.signalFrameCount = 0;
        s.signalFinalFrameDrawn = false;
        hal::display::markAllDirty();
    }
    if (elapsed >= kSignalDurationMs) {
        if (s.signalFinalFrameDrawn) return;
        s.signalFinalFrameDrawn = true;
    } else if (s.signalFrameCount > 0 && now - s.signalLastDrawMs < kSignalFrameIntervalMs) {
        return;
    }
    s.signalLastDrawMs = now;
    const float progress = elapsed <= kSignalLeadInMs
        ? 0.f
        : signalClamp((float)(elapsed - kSignalLeadInMs) /
                      (float)kSignalMotionDurationMs);
    const uint32_t renderStartUs = micros();
    drawSignalBoot(canvas, progress);
    const uint32_t renderUs = micros() - renderStartUs;
    const uint32_t renderedAtMs = millis();
    if (!s.timingMotionLogged && progress > 0.f) {
        s.timingMotionLogged = true;
        Serial.printf("[boot-timing] event=motion_first_frame millis=%lu elapsed=%lu progress_permille=%u render_us=%lu\n",
                      (unsigned long)renderedAtMs, (unsigned long)elapsed,
                      (unsigned)lroundf(progress * 1000.f),
                      (unsigned long)renderUs);
    }
    if (!s.timingLogoVisibleLogged && progress >= 0.76f) {
        s.timingLogoVisibleLogged = true;
        Serial.printf("[boot-timing] event=logo_first_frame millis=%lu elapsed=%lu progress_permille=%u render_us=%lu\n",
                      (unsigned long)renderedAtMs, (unsigned long)elapsed,
                      (unsigned)lroundf(progress * 1000.f),
                      (unsigned long)renderUs);
    }
    if (!s.timingLogoFullLogged && progress >= 0.96f) {
        s.timingLogoFullLogged = true;
        Serial.printf("[boot-timing] event=logo_full_frame millis=%lu elapsed=%lu progress_permille=%u render_us=%lu\n",
                      (unsigned long)renderedAtMs, (unsigned long)elapsed,
                      (unsigned)lroundf(progress * 1000.f),
                      (unsigned long)renderUs);
    }
    if (!s.timingMotionCompleteLogged && elapsed >= kSignalDurationMs) {
        s.timingMotionCompleteLogged = true;
        Serial.printf("[boot-timing] event=motion_complete_frame millis=%lu elapsed=%lu render_us=%lu\n",
                      (unsigned long)renderedAtMs, (unsigned long)elapsed,
                      (unsigned long)renderUs);
    }
    ++s.signalFrameCount;
    s.signalRenderTotalUs += renderUs;
    s.signalRenderMaxUs = max(s.signalRenderMaxUs, renderUs);
}

static void buildSignalBootScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("boot.signal.bg", {0, 0, width, height}, kSignalPaper, 0, 0);
    scene.addLabel("boot.signal.sequence", {0, (int16_t)(height / 2 - 12), width, 24},
                   "Clean Pull", kSignalInk, kSignalPaper, 2,
                   ui::scene::TextAlign::TopCenter, 1);
}

// Boot screens consume the exit chord while their animation is active.

static bool logoOnChord() { return true; }

// ─── Screen: presents hold ───────────────────────────────────────────────────

static void presentsOnEnter() {
    s.phaseStartMs = millis();
    bindBootAudio();
    Serial.println("[boot] presents screen");
    if (!s.logoReady) preloadLogoFrames();
    if (!s.eyesReady) preloadHomeEyesFrame();
    buildLogoScene();
    buildPresentsNodes();
    buildEyeMorphNodes();
    rootGroup.add(&presentsGroup.base);
    rootGroup.add(&homeEyesSprite.base);
    layoutScene(hal::display::canvas());

    {
        presentsLabel.opacity.animate(1.f, ::scene::makeTransition(400, 0, ::scene::Ease::EaseOut), millis());
    }
    {
        kamiLabel.opacity.animate(1.f, ::scene::makeTransition(400, 80, ::scene::Ease::EaseOut), millis());
    }
    soundPresents.playFromDef(sfx::kSfxBootPresents, s.phaseStartMs);
    hal::display::markAllDirty();
}

static void presentsOnExit() {
    releaseBootAudio();
    presentsGroup.clear();
}

static void presentsOnTick(uint32_t now) {
    if (now - s.phaseStartMs >= BOOT_PRESENTS_MS) {
        ::scene::switchTo(&app, &kScreenEyeMorph, ::scene::ScreenTransition::Silent);
    }
}

static void presentsOnDraw(TFT_eSprite& canvas) { bootDrawContent(canvas); }

// ─── Screen: mini-eye morph ──────────────────────────────────────────────────

static void eyeMorphOnEnter() {
    s.phaseStartMs = millis();
    bindBootAudio();
    if (!s.eyesReady) preloadHomeEyesFrame();
    initGroupNode(rootGroup, true);
    rootGroup.clear();
    buildEyeMorphNodes();
    rootGroup.add(&homeEyesSprite.base);
    hal::display::markAllDirty();
}

static void eyeMorphOnExit() {
    releaseBootAudio();
}

static void eyeMorphOnTick(uint32_t now) {
    const uint32_t elapsed = now - s.phaseStartMs;
    const float    u       = (elapsed >= BOOT_MORPH_MS) ? 1.f : (float)elapsed / (float)BOOT_MORPH_MS;

    placeHomeEyesForProgress(u);

    if (elapsed >= BOOT_MORPH_MS) {
        soundEyeLand.playFromDef(sfx::kSfxBootEyeLand, now);
        Serial.println("[boot] handoff screen");
        ::scene::switchTo(&app, &kScreenHandoff, ::scene::ScreenTransition::Silent);
    }
}

static void eyeMorphOnDraw(TFT_eSprite& canvas) { bootDrawContent(canvas); }

// ─── Screen: flash → pop to Kami ─────────────────────────────────────────────

static void handoffOnEnter() {
    s.phaseStartMs = millis();
    bindBootAudio();
    if (!s.eyesReady) preloadHomeEyesFrame();
    initGroupNode(rootGroup, true);
    rootGroup.clear();
    buildEyeMorphNodes();
    placeHomeEyesForProgress(1.f);
    rootGroup.add(&homeEyesSprite.base);
    hal::display::markAllDirty();

    hal::display::releaseSpiForSd();
    TFT_eSPI& tft = hal::display::tft();
    if (!ui::home_eyes::isLoaded()) {
        (void)ui::home_eyes::beginSession(tft);
    }
}

static void handoffOnExit() {
    releaseBootAudio();
    clearScene();
    presentsGroup.clear();
}

static void handoffOnTick(uint32_t now) {
    const uint32_t elapsed = now - s.phaseStartMs;
    if (elapsed < kBootHandoffSettleMs) return;

    Serial.println("[boot] pop -> Kami");
    ui::kami_motion::trigger(ui::kami_expression::Action::Wake,now);
    NavHandler::instance().pop();
}

static void handoffOnDraw(TFT_eSprite& canvas) { bootDrawContent(canvas); }

// ─── Screen tables ───────────────────────────────────────────────────────────

const ::scene::Screen kScreenBoot = {
    "BootDoubleBlink",
    signalBootOnEnter,
    signalBootOnExit,
    signalBootOnTick,
    nullptr,
    logoOnChord,
    signalBootOnDraw,
    "boot",
    buildSignalBootScene,
};

const ::scene::Screen kScreenPresents = {
    "BootPresents",
    presentsOnEnter,
    presentsOnExit,
    presentsOnTick,
    nullptr,
    logoOnChord,
    presentsOnDraw,
    "presents",
    buildBootPresentsScene,
    &screen_chrome_layout::kBootPresentsTopBarColor,
};

const ::scene::Screen kScreenEyeMorph = {
    "BootEyeMorph",
    eyeMorphOnEnter,
    eyeMorphOnExit,
    eyeMorphOnTick,
    nullptr,
    logoOnChord,
    eyeMorphOnDraw,
    "eyes",
    buildBootEyesScene,
    &screen_chrome_layout::kBootEyesTopBarColor,
};

const ::scene::Screen kScreenHandoff = {
    "BootHandoff",
    handoffOnEnter,
    handoffOnExit,
    handoffOnTick,
    nullptr,
    logoOnChord,
    handoffOnDraw,
    "handoff",
    buildBootHandoffScene,
    &screen_chrome_layout::kBootHandoffTopBarColor,
};

}  // namespace boot
}  // namespace apps
