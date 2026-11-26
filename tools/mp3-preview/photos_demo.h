#pragma once
// Interactive Photos demo for Kami Screen Studio.
//
// Replays the studio's control events through apps/photos_flow.h (the same
// state machine the firmware's photos_app.cpp uses) over a fixture library,
// then paints the frame at `now` with ui/photos_view.h. Hardware effects are
// stand-ins: Delete removes a fixture, Upload marks the fixture queued and
// shows the same notice as the device, and Play shows a placeholder for the
// Videos app. Stateless: every request replays the full event list.
#include <cstdlib>
#include <cstring>
#include "apps/photos_flow.h"
#include "photos_study.h"
#include "photos_studio.h"

namespace photos_demo {

using namespace apps::photos;
using flow::Command;
using flow::Mode;
namespace pv = ui::photos_view;

struct Fixture { photos_study::Sample sample; bool video; const char* duration; bool uploaded; bool queued;
                 const char* date; const char* time; };  // null date: no capture time (videos have none yet)

constexpr int kMaxItems = 6;
constexpr uint32_t kNoticeMs = 1800;   // as photos_app.cpp

struct Sim {
    flow::Model m;
    Fixture items[kMaxItems] = {
        {photos_study::Sample::Sunset, false, "", false, false, "24 Nov", "4:12 pm"},
        {photos_study::Sample::Street, true, "0:12", false, false, nullptr, nullptr},
        {photos_study::Sample::Lake, false, "", true, false, "22 Nov", "7:05 pm"},
        {photos_study::Sample::Street, true, "1:05", true, false, nullptr, nullptr},
        {photos_study::Sample::Lake, false, "", false, false, nullptr, nullptr},
        {photos_study::Sample::Sunset, false, "", true, false, "3 Jan 2025", "9:41 am"},
    };
    uint32_t noticeUntil = 0;
    const char* noticeText = "";
    bool playing = false, exited = false;
    const char* last = "Opened Photos";

    Sim() { flow::loaded(m, kMaxItems); }
    const Fixture& current() const { return items[m.index]; }

    void advance(uint32_t now) {
        if (noticeUntil && now >= noticeUntil) noticeUntil = 0;
    }

    void input(char kind, int value, uint32_t at) {
        if (exited) { *this = Sim(); last = "Reopened Photos"; return; }
        if (playing) {  // stand-in for the Videos app; LB returns
            if (kind == 'a') { playing = false; last = "Back from the Videos app"; }
            return;
        }
        const flow::Input in = kind == 'a' ? flow::Input::Back : kind == 'b' ? flow::Input::Right
                             : kind == 'c' ? flow::Input::Press : flow::Input::Turn;
        const flow::Item item = m.count ? flow::Item{current().video, current().uploaded} : flow::Item{false, false};
        switch (flow::apply(m, in, value, item, true)) {
            case Command::None: last = "No effect in this state"; break;
            case Command::Repaint: last = "Moved focus"; break;
            case Command::RedrawImage:
                last = m.mode == Mode::Confirm ? "Asked to confirm delete"
                     : m.mode == Mode::Sheet   ? "Back to the sheet" : "Closed the sheet";
                break;
            case Command::OpenSheet: last = "Opened the action sheet"; noticeUntil = 0; break;
            case Command::Browse: noticeUntil = 0; last = "Moved to another item"; break;
            case Command::Play: playing = true; noticeUntil = 0; last = "Playing video (Videos app)"; break;
            case Command::Upload:
                noticeText = items[m.index].queued ? "Already  in  upload  queue" : "Added  to  upload  queue";
                items[m.index].queued = true;
                flow::uploadQueued(m);
                noticeUntil = at + kNoticeMs;
                last = "Added to the upload queue";
                break;
            case Command::Delete:
                for (int i = m.index; i + 1 < m.count; ++i) items[i] = items[i + 1];
                flow::removed(m, m.count - 1);
                last = "Deleted";
                break;
            case Command::Exit: exited = true; last = "Left Photos"; break;
            case Command::Reload: flow::loaded(m, m.count); last = "Reloaded"; break;
        }
    }

    static void c_fill(photos_study::Raster& r, const photos_study::Media& media) {
        r.fillRect(0, 0, photos_study::W, photos_study::H, photos_study::kBlack);
        photos_study::coverSample(r, media.sample);
    }

    void paint(photos_study::Raster& r) const {
        using namespace photos_study;
        const pv::Bounds s{W, H};
        if (exited) { pv::message(r, s, "Left Photos", "On the device this returns to the menu.", "Press any control to reopen."); return; }
        if (m.mode == Mode::Empty) { proposedEmpty(r); return; }
        const Fixture& f = current();
        const Media media{f.sample, f.video, f.duration, f.uploaded};
        if (playing) {
            c_fill(r, media);
            pv::roundRect(r, s, 40, 96, 200, 48, 14, pv::kSheet, 235);
            pv::textCenter(r, s, "Videos  app  plays  this  file", W / 2, 106, 12, pv::kWhite);
            pv::textCenter(r, s, "Press  A  to  come  back", W / 2, 124, 12, pv::kMuted);
            return;
        }
        // As photos_app.cpp: cover-cropped media, index pill, date caption.
        c_fill(r, media);
        if (f.video) pv::playBadge(r, s, W / 2, H / 2);
        pv::indexPill(r, s, m.index + 1, m.count);
        if (m.mode == Mode::Browse) {
            if (noticeUntil) pv::notice(r, s, noticeText, true);
            else {
                const char* parts[] = {f.date, f.time, f.video ? f.duration : nullptr};
                pv::dateCaption(r, s, parts, 3);
            }
            return;
        }
        if (m.mode == Mode::Sheet) {
            pv::SheetRow rowsBuf[3];
            for (int i = 0; i < m.sheet.count; ++i) {
                const auto a = m.sheet.at(i);
                rowsBuf[i] = a == policy::Action::Play   ? pv::SheetRow{"Play", pv::Icon::Play, false}
                           : a == policy::Action::Upload ? pv::SheetRow{"Upload", pv::Icon::Upload, false}
                                                         : pv::SheetRow{"Delete", pv::Icon::Trash, true};
            }
            pv::sheet(r, s, pv::SheetModel{f.video ? "Video" : "Photo", m.index + 1, m.count, f.uploaded, nullptr,
                                           rowsBuf, m.sheet.count, m.row}, true);
            return;
        }
        if (m.mode == Mode::Confirm) pv::confirmDelete(r, s, f.video, m.confirmDelete, true);
    }

    bool animating() const { return noticeUntil != 0; }
    const char* modeName() const {
        if (exited) return "exited";
        if (playing) return "playing video";
        switch (m.mode) {
            case Mode::Browse: return "browse";
            case Mode::Sheet: return "sheet";
            case Mode::Confirm: return "confirm";
            case Mode::Empty: return "empty";
            case Mode::Error: return "error";
        }
        return "-";
    }
};

// events: "at,kind,value;..." with kind a (LB tap), b (RB tap), c (C press), r (turn, value=delta).
template <class Out> void run(Out& out, const char* events, uint32_t now) {
    static Sim sim;
    sim = Sim();
    for (const char* p = events; *p;) {
        char* end = nullptr;
        const uint32_t at = strtoul(p, &end, 10);
        if (*end != ',') break;
        const char kind = end[1];
        if (end[2] != ',') break;
        const int value = (int)strtol(end + 3, &end, 10);
        sim.advance(at);
        sim.input(kind, value, at);
        p = *end == ';' ? end + 1 : end;
    }
    sim.advance(now);
    static photos_study::Raster raster;
    raster = {};
    sim.paint(raster);
    printf("{\"pixels\":[");
    photos_studio::emit(out, raster);
    printf("],\"mode\":\"%s\",\"index\":%d,\"count\":%d,\"focus\":\"%s\",\"animating\":%s,\"last\":\"%s\"}\n",
           sim.modeName(), sim.m.count ? sim.m.index + 1 : 0, sim.m.count, sim.m.focusName(),
           sim.animating() ? "true" : "false", sim.last);
}

}  // namespace photos_demo
