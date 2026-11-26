#pragma once
// Photos input state machine, shared by the firmware app and Kami Screen
// Studio's interactive Photos demo so both respond to the same controls in the
// same way. Pure: no I/O, no drawing. apply() updates the model and returns
// the one side effect the caller must perform (delete, upload, play, exit...).
// Callers that perform an effect report its outcome with the helpers below.
#include "apps/photos_policy.h"

namespace apps {
namespace photos {
namespace flow {

enum class Mode : uint8_t { Browse, Sheet, Confirm, Empty, Error };
enum class Input : uint8_t { Back, Right, Press, Turn };  // LB tap, RB tap, C press, C rotation

enum class Command : uint8_t {
    None,
    Repaint,      // focus moved inside an overlay; repaint the overlay only
    RedrawImage,  // overlay closed or replaced; redraw media (then any overlay)
    OpenSheet,    // sheet opened over the current image (scrim + sheet)
    Browse,       // index changed; redraw media and show the position readout
    Play,         // open the current video in the player
    Upload,       // add the current item to the upload queue, then report uploadQueued/uploadFailed
    Delete,       // delete the current item, then report removed/deleteFailed
    Exit,         // leave Photos
    Reload,       // retry loading the library
};

struct Item { bool video; bool uploaded; };

struct Model {
    Mode mode = Mode::Empty;
    int index = 0, count = 0;            // index 0 = newest
    policy::Sheet sheet = policy::sheetFor(false);
    int row = 0;
    bool confirmDelete = false;          // confirm focus: false = Cancel

    bool viewing() const { return mode == Mode::Browse || mode == Mode::Sheet || mode == Mode::Confirm; }
    bool overlay() const { return mode == Mode::Sheet || mode == Mode::Confirm; }
    policy::Action focused() const { return sheet.at(row); }
    const char* focusName() const {
        return mode == Mode::Confirm ? (confirmDelete ? "delete" : "cancel")
             : mode == Mode::Sheet   ? policy::actionName(focused()) : "-";
    }
};

inline Command openSheet(Model& m, Item item) {
    m.sheet = policy::sheetFor(item.video);  // item.uploaded only labels the header
    m.row = 0;                 // Play / Upload first; Delete is never the default
    m.mode = Mode::Sheet;
    return Command::OpenSheet;
}

// ready: the last focus change has been drawn. A press must follow a rendered
// selection, so a buffered press cannot activate a choice the user never saw.
inline Command apply(Model& m, Input in, int delta, Item current, bool ready) {
    switch (m.mode) {
        case Mode::Confirm:
            if (in == Input::Back) { m.mode = Mode::Sheet; return Command::RedrawImage; }
            if (in == Input::Turn && delta) {
                const bool next = policy::moveConfirm(m.confirmDelete, delta);
                if (next == m.confirmDelete) return Command::None;
                m.confirmDelete = next;
                return Command::Repaint;
            }
            if (in == Input::Press && ready) {
                if (m.confirmDelete) return Command::Delete;
                m.mode = Mode::Sheet;
                return Command::RedrawImage;
            }
            return Command::None;

        case Mode::Sheet:
            if (in == Input::Back) { m.mode = Mode::Browse; return Command::RedrawImage; }
            if (in == Input::Turn && delta) {
                const int next = policy::moveRow(m.row, delta, m.sheet.count);
                if (next == m.row) return Command::None;
                m.row = next;
                return Command::Repaint;
            }
            if (in == Input::Press && ready) {
                switch (m.focused()) {
                    case policy::Action::Play:   m.mode = Mode::Browse; return Command::Play;
                    case policy::Action::Upload: return Command::Upload;
                    case policy::Action::Delete:
                        m.mode = Mode::Confirm;
                        m.confirmDelete = false;  // Cancel is the default
                        return Command::RedrawImage;
                }
            }
            return Command::None;

        case Mode::Empty:
        case Mode::Error:
            if (in == Input::Back) return Command::Exit;
            if (in == Input::Press) return Command::Reload;
            return Command::None;

        case Mode::Browse:
            if (in == Input::Back) return Command::Exit;
            // C opens the action sheet for photos and videos alike.
            if (in == Input::Press && ready) return openSheet(m, current);
            // RB plays the video under the cursor; on a photo it does nothing.
            if (in == Input::Right && ready) return current.video ? Command::Play : Command::None;
            if (in == Input::Turn && delta) {
                const int next = policy::moveIndex(m.index, delta, m.count);
                if (next == m.index) return Command::None;
                m.index = next;
                return Command::Browse;
            }
            return Command::None;
    }
    return Command::None;
}

// --- Outcomes of effects ----------------------------------------------------

inline void loaded(Model& m, int count, int index = 0) {
    m.count = count;
    m.index = count ? (index < 0 ? 0 : index >= count ? count - 1 : index) : 0;
    m.mode = count ? Mode::Browse : Mode::Empty;
}
inline void loadFailed(Model& m) { m.count = 0; m.index = 0; m.mode = Mode::Error; }
inline void removed(Model& m, int remaining) {
    m.count = remaining;
    m.index = policy::indexAfterRemoval(m.index, remaining);
    m.mode = remaining ? Mode::Browse : Mode::Empty;
}
inline void deleteFailed(Model& m) { m.mode = Mode::Sheet; m.row = 0; }
inline void uploadQueued(Model& m) { m.mode = m.count ? Mode::Browse : Mode::Empty; }  // keep browsing
inline void uploadFailed(Model& m) { m.mode = Mode::Sheet; }
inline void closed(Model& m) { m.mode = Mode::Empty; m.count = 0; m.index = 0; }

}  // namespace flow
}  // namespace photos
}  // namespace apps
