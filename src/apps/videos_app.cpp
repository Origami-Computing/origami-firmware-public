#include "apps/videos_app.h"
#include "services/video_library.h"
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
#include "services/camera_preview_decode.h"
#endif
#include "ui/camera_preview.h"
#include "ui/scene.h"
#include "ui/nav_controls.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "config/ui_theme.h"
#include <Arduino.h>
#include <stdio.h>
#include <algorithm>
#include <string.h>

namespace apps { namespace videos {
static services::video::Library s_library;
static ui::CameraPreview s_preview;
static int s_selected=0;
static bool s_frame=false,s_playing=false,s_ended=false,s_dirty=true,s_failed=false;
static bool s_audioTail=false;
static uint32_t s_number=0,s_startedMs=0,s_elapsedMs=0,s_skipped=0;
static uint32_t s_decodeMaxMs=0,s_presentMaxMs=0;
static uint32_t s_imageNumber=0,s_presentedMs=0,s_decoded=0,s_held=0;
static uint32_t s_tickExit=0,s_tickOutside=0,s_tickWork=0,s_tickCalls=0,s_tickGapMax=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static uint32_t s_profileFrames=0,s_profilePresentUs=0,s_profileSyncUs=0,s_profileTransferUs=0,s_profileSyncMisses=0;
#endif
static const char* s_message="Loading videos...";
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
static services::camera::PreviewDecode s_decode;
static bool s_decodeTried=false,s_asyncDecode=false,s_decodePending=false;
static uint32_t s_pendingNumber=0,s_pendingImage=0;
static void resetDecode() {
    s_decode.close();s_decodeTried=s_asyncDecode=s_decodePending=false;
}
#endif

static uint32_t playbackTimeMs(uint32_t now) {
    if (s_frame && s_library.info().audioSamples && !s_audioTail) {
        const uint32_t delivered=s_library.audioPositionMs();
        const uint32_t duration=uint64_t(s_library.info().audioSamples)*1000/core::avi::kAudioRate;
        if (delivered<duration) return delivered;
        // Preserve video after a shorter audio stream. Anchor this tail only
        // when delivered audio finishes, so SD stalls do not become a jump.
        s_audioTail=true;s_elapsedMs=duration;s_startedMs=now;
    }
    return s_elapsedMs + (s_playing ? uint32_t(now-s_startedMs) : 0);
}
static bool setPlaying(bool playing) {
    const uint32_t elapsed=playbackTimeMs(millis());
    if (playing && !s_library.startAudio()) return false;
    if (!playing) s_library.pauseAudio();
    s_elapsedMs=elapsed;s_startedMs=millis();s_playing=playing;
    return true;
}
static void failPlayback(const char* message) {
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
    resetDecode();
#endif
    s_message=message;s_failed=true;s_playing=s_frame=false;s_dirty=true;
    s_library.stop();hal::display::exitOptimizedRender();hal::display::markAllDirty();
    NavHandler::instance().runtimePolicyChanged();
}
static bool finishIfDone() {
    if (!s_playing || !s_frame) return false;
    const auto& info=s_library.info();
    // Presenting the last frame starts its interval; it does not finish it.
    // In particular, a one-frame preview must remain paused until Play.
    const uint32_t durationMs=uint32_t((uint64_t(info.frames)*info.intervalUs+999)/1000);
    if (s_number+1!=info.frames ||
        playbackTimeMs(millis())<durationMs ||
        (info.audioSamples && s_library.audioPositionMs()<uint64_t(info.audioSamples)*1000/core::avi::kAudioRate)) return false;
    setPlaying(false);s_ended=true;
    return true;
}

static void present() {
    if (!s_frame) return;
    const auto& info=s_library.info();
    const uint32_t durationMs=std::max(uint32_t((uint64_t(info.frames)*info.intervalUs+999)/1000),
        uint32_t(uint64_t(info.audioSamples)*1000/core::avi::kAudioRate));
    const uint32_t total=(durationMs+999)/1000;
    const uint32_t elapsed=s_ended?total:playbackTimeMs(millis())/1000;
    const char* name=strrchr(s_library.path(s_selected),'/');
    char title[40],controls[40];
    snprintf(title,sizeof(title),"%d/%d %s",s_selected+1,s_library.count(),name?name+1:"");
    snprintf(controls,sizeof(controls),"B:%s %02lu:%02lu/%02lu:%02lu",
        s_ended?"REPLAY":s_playing?"PAUSE":"PLAY",
        (unsigned long)(elapsed/60),(unsigned long)(elapsed%60),
        (unsigned long)(total/60),(unsigned long)(total%60));
    const uint32_t started=millis();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    const uint32_t profileStarted=micros();
#endif
    const auto presentation=s_preview.presentVideo(title,controls,s_ended?1000:durationMs ? uint64_t(playbackTimeMs(millis()))*1000/durationMs : 0);
    (void)presentation;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    ++s_profileFrames;s_profilePresentUs+=uint32_t(micros()-profileStarted);
    s_profileSyncUs+=presentation.syncWaitUs;s_profileTransferUs+=presentation.transferUs;
    s_profileSyncMisses+=presentation.syncMissed;
    if (s_profileFrames==16) {
        Serial.printf("[video-present] frames=%lu total_us=%lu sync_us=%lu transfer_us=%lu sync_misses=%lu\n",
            (unsigned long)s_profileFrames,(unsigned long)s_profilePresentUs,
            (unsigned long)s_profileSyncUs,(unsigned long)s_profileTransferUs,(unsigned long)s_profileSyncMisses);
        s_profileFrames=s_profilePresentUs=s_profileSyncUs=s_profileTransferUs=s_profileSyncMisses=0;
    }
#endif
    s_presentMaxMs=std::max(s_presentMaxMs,uint32_t(millis()-started));
    s_presentedMs=millis();
}
static void selectClip(bool autoplay=false) {
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
    resetDecode();s_preview.closeDecoder();
#endif
    hal::display::exitOptimizedRender();
    s_frame=false;s_playing=autoplay;s_ended=false;s_number=0;s_dirty=true;s_failed=false;
    s_elapsedMs=s_skipped=s_decoded=s_held=0;s_startedMs=millis();s_audioTail=false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    s_profileFrames=s_profilePresentUs=s_profileSyncUs=s_profileTransferUs=s_profileSyncMisses=0;
#endif
    s_tickExit=s_tickOutside=s_tickWork=s_tickCalls=s_tickGapMax=0;
    s_message="Loading preview...";
    if (!s_library.select(s_selected)) {s_message=s_library.error();s_playing=false;s_failed=true;}
    hal::display::markAllDirty();
}
static void close() {
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
    resetDecode();
#endif
    s_library.close();
    s_preview.close();
    hal::display::exitOptimizedRender();
    s_frame=s_playing=false;
}
static void open() {
    s_frame=s_playing=s_ended=s_failed=false;s_dirty=true;
    s_decodeMaxMs=s_presentMaxMs=0;
    if (!s_library.open(true)) {
        s_message=s_library.error();s_failed=!s_library.opening();
        hal::display::markAllDirty();return;
    }
    if (!s_library.count()) {s_message="No videos yet. Record in Camera.";return;}
    bool prepared=s_preview.prepareSurfaces();
#ifndef ORIGAMI_ASYNC_VIDEO_PLAYBACK
    prepared=prepared&&s_preview.prepareDecoder(true);
#endif
    if (!prepared) {
        close();s_message="Not enough video memory";s_failed=true;return;
    }
    if (s_selected>=s_library.count()) s_selected=s_library.count()-1;
    selectClip();
}
static void enter() {s_selected=0;open();}
static void drawScene(ui::scene::Scene& scene,int16_t w,int16_t h) {
    scene.clear();ui::scene::setCurrentScene(&scene);
    ui::scene::Builder b(scene,w,h);b.clearContent(kUiBg);
    b.panelHeader("videos.title","Videos");
    b.labelCenterMuted("videos.message",s_message);
}
static void draw(TFT_eSprite& canvas) {
    if (!s_dirty || s_frame) return;
    s_dirty=false;
    auto& scene=ui::scene::scratchScene();drawScene(scene,canvas.width(),canvas.height());
    scene.render(canvas);hal::display::markAllDirty();
}
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
static void tickAsync(uint32_t now) {
    if (s_decode.failed()) {failPlayback("Cannot decode video");return;}
    if (s_frame&&!s_playing) return;
    if (finishIfDone()) {present();NavHandler::instance().runtimePolicyChanged();return;}
    const auto& info=s_library.info();
    bool paint=false;
    services::camera::PreviewDecode::Frame decoded{};
    if (s_decodePending&&s_decode.acquire(decoded)) {
        const bool copied=decoded.sensorUs==uint64_t(s_pendingNumber)*info.intervalUs &&
            s_preview.copyDecoded(decoded.pixels,true);
        s_decode.release();s_decodePending=false;
        if (!copied) {failPlayback("Cannot present video");return;}
        s_decodeMaxMs=std::max(s_decodeMaxMs,(decoded.decodeUs+999)/1000);
        if (s_frame&&s_pendingNumber>s_number) s_skipped+=s_pendingNumber-s_number-1;
        if (!s_frame) {
            s_startedMs=millis();hal::display::enterOptimizedRender();
            if (s_playing&&!s_library.startAudio()) {failPlayback(s_library.error());return;}
        }
        s_frame=true;s_number=s_pendingNumber;s_imageNumber=s_pendingImage;++s_decoded;paint=true;
    }
    // One outstanding frame bounds latency and metadata ownership. Submit the
    // next due JPEG before the UI-owned panel transfer, so decode can overlap it.
    const uint32_t target=s_frame&&info.intervalUs ?
        std::min(uint32_t(info.frames-1),uint32_t(uint64_t(playbackTimeMs(millis()))*1000/info.intervalUs)) : 0;
    if (!s_decodePending && (!s_frame||s_playing) && (!s_frame||target>s_number)) {
        services::video::Library::Frame frame;
        if (s_library.take(frame,target)) {
            if (s_frame&&frame.number+1<info.frames&&uint64_t(target-frame.number)*info.intervalUs>100000) {
                s_library.release();
            } else if (s_frame&&frame.imageNumber==s_imageNumber) {
                if (frame.number>s_number) s_skipped+=frame.number-s_number-1;
                s_number=frame.number;++s_held;s_library.release();
                paint=paint||uint32_t(now-s_presentedMs)>=500;
            } else {
                s_pendingNumber=frame.number;s_pendingImage=frame.imageNumber;
                const bool queued=s_decode.submit(frame.data,frame.length,uint64_t(frame.number)*info.intervalUs);
                s_library.release();
                if (!queued) {failPlayback("Cannot queue video decode");return;}
                s_decodePending=true;
            }
        }
    }
    if (finishIfDone()) paint=true;
    if (paint) {present();NavHandler::instance().runtimePolicyChanged();}
}
#endif
static void tick(uint32_t now) {
    if (s_library.opening()) {open();return;}
    const uint32_t entered=millis();
    if(s_tickExit){const uint32_t gap=entered-s_tickExit;s_tickOutside+=gap;s_tickGapMax=std::max(s_tickGapMax,gap);}
    struct TickDuration {
        uint32_t entered;
        ~TickDuration(){const uint32_t end=millis();s_tickWork+=end-entered;s_tickExit=end;++s_tickCalls;}
    } duration{entered};
    if (s_failed || !s_library.count()) return;
    if (s_library.state()==services::video::Library::State::Failed) {
        failPlayback(s_library.error());return;
    }
    if (s_library.audioFailed()) {failPlayback("Cannot read video audio");return;}
    // The worker publishes metadata with Ready/End; do not read it while it is
    // still parsing the selected file on the other core.
    if (s_library.state()==services::video::Library::State::Loading ||
        s_library.state()==services::video::Library::State::Idle) return;
#ifdef ORIGAMI_ASYNC_VIDEO_PLAYBACK
    if (!s_decodeTried) {
        s_decodeTried=true;s_asyncDecode=s_decode.begin(true);
        Serial.printf("[video-decode-worker] enabled=%u\n",unsigned(s_asyncDecode));
        if (!s_asyncDecode&&!s_preview.prepareDecoder(true)) {failPlayback("Not enough video memory");return;}
    }
    if (s_asyncDecode) {tickAsync(now);return;}
#endif
    if (s_frame && !s_playing) return;
    if (finishIfDone()) {present();NavHandler::instance().runtimePolicyChanged();return;}
    const auto& info=s_library.info();
    const uint32_t target=s_frame && info.intervalUs ?
        std::min(uint32_t(info.frames-1),uint32_t(uint64_t(playbackTimeMs(now))*1000/info.intervalUs)) : 0;
    if (s_frame && target<=s_number) return;
    services::video::Library::Frame frame;
    // Consume the newest frame already due, not a future image. Requiring
    // an exact/current-clock frame can starve rendering when prefetch is late.
    if (!s_library.take(frame,target)) return;
    // A late reader may only have old frames published. Allow modest jitter,
    // but drain that backlog without decoding so video cannot trail audio.
    if (s_frame && frame.number+1<info.frames &&
        uint64_t(target-frame.number)*info.intervalUs>100000) {
        s_library.release();return;
    }
    if (s_frame && frame.number>s_number) s_skipped+=frame.number-s_number-1;
    const uint32_t number=frame.number;
    if (s_frame && frame.imageNumber==s_imageNumber) {
        s_library.release();s_number=number;++s_held;
        const bool ended=finishIfDone();
        // The retained image is already correct. Refresh progress at 2 Hz,
        // avoiding JPEG decode and full-panel transfers for every held tick.
        if (ended || uint32_t(now-s_presentedMs)>=500) present();
        if (ended) NavHandler::instance().runtimePolicyChanged();
        return;
    }
    const uint32_t imageNumber=frame.imageNumber;
    const uint32_t decodeStarted=millis();
    const bool decoded=s_preview.decodeVideo(frame.data,frame.length)==ui::CameraPreview::Result::Decoded;
    s_decodeMaxMs=std::max(s_decodeMaxMs,uint32_t(millis()-decodeStarted));
    s_library.release();
    if (!decoded) {failPlayback("Cannot decode video");return;}
    if (!s_frame) {
        s_startedMs=millis();hal::display::enterOptimizedRender();
        if (s_playing && !s_library.startAudio()) {failPlayback(s_library.error());return;}
    }
    s_frame=true;s_number=number;s_imageNumber=imageNumber;++s_decoded;
    finishIfDone();
    present();
    NavHandler::instance().runtimePolicyChanged();
}
static void input(hal::input::InputEvent event) {
    using E=hal::input::EventType;
    if (ui::nav::isBackTap(event)) {NavHandler::instance().pop();return;}
    if (event.type==E::Scroll && s_library.count()) {
        const int next=std::max(0,std::min(s_library.count()-1,s_selected+int(event.delta)));
        if (next!=s_selected) {s_selected=next;selectClip();}
    } else if (ui::nav::isEnterTap(event) || event.type==E::ButtonB_Tap) {
        if (s_ended || s_failed) {if(s_library.count())selectClip(true);else open();}
        else if (s_frame) {
            if (!setPlaying(!s_playing)) {failPlayback(s_library.error());return;}
            present();
        }
    }
    NavHandler::instance().runtimePolicyChanged();
}
void dumpStatus(Stream& output) {
    output.printf("[video-loop] calls=%lu work_ms=%lu outside_ms=%lu gap_max_ms=%lu\n",
        (unsigned long)s_tickCalls,(unsigned long)s_tickWork,(unsigned long)s_tickOutside,(unsigned long)s_tickGapMax);
    output.printf("[video-render] decoded=%lu held=%lu\n",(unsigned long)s_decoded,(unsigned long)s_held);
    if (s_failed) output.printf("[videos-error] %s\n",s_message);
    output.printf("[videos] active=%d ready=%d playing=%d ended=%d error=%d index=%d count=%d frame=%lu total=%lu audio=%d audio_ms=%lu skipped=%lu elapsed_ms=%lu decode_max_ms=%lu present_max_ms=%lu path=%s\n",
        NavHandler::instance().top()==&app,s_frame,s_playing,s_ended,s_failed,s_selected+1,s_library.count(),
        (unsigned long)(s_frame?s_number+1:0),(unsigned long)(s_frame?s_library.info().frames:0),
        s_frame && s_library.info().audioSamples>0,(unsigned long)s_library.audioPositionMs(),
        (unsigned long)s_skipped,(unsigned long)playbackTimeMs(millis()),
        (unsigned long)s_decodeMaxMs,(unsigned long)s_presentMaxMs,s_library.path(s_selected));
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool copyPreviewForDevelopment(TFT_eSprite& target) {
    return NavHandler::instance().top()==&app && s_frame && !s_failed && !s_playing &&
        s_preview.copyCompositeForDevelopment(target);
}
#endif
static AppPresentation presentation() {
    return s_frame ? AppPresentation{true,false,AppPresentation::StatusBar::Hidden} : AppPresentation{};
}
static core::health::RuntimePolicy runtime() {
    core::health::RuntimePolicy policy{};policy.reportActivity=s_playing || s_library.opening();return policy;
}
static void refresh() {s_dirty=true;if(s_frame)present();}
static const ::scene::Screen screen={"Videos",refresh,nullptr,tick,input,nullptr,draw,"browse",drawScene};
static const ::scene::Screen* const screens[]={&screen};
App app={"Videos",&screen,nullptr,nullptr,close,"videos","src/apps/videos_app.cpp",screens,1,
    presentation,runtime,enter,open,nullptr,close};
} }
