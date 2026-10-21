#include "scene/screen.h"

#include "app.h"
#include "sfx/audio_group.h"
#include "services/vibration.h"

namespace scene {

void switchTo(App* app, const Screen* screen) { switchTo(app,screen,ScreenTransition::Automatic); }

void switchTo(App* app, const Screen* screen, ScreenTransition direction) {
    if (!app || !screen) return;
    const bool changed=app->currentScreen!=screen;
    if(changed) {
        using namespace services::vibration;
        if(direction==ScreenTransition::Automatic)screenFeedback();
        else if(direction==ScreenTransition::Forward)navigationFeedback(Cue::Select);
        else if(direction==ScreenTransition::Back)navigationFeedback(Cue::Back);
    }
    if (app->currentScreen && app->currentScreen->onExit) app->currentScreen->onExit();
    sfx::setActiveAudioGroup(nullptr);
    app->currentScreen = screen;
    if (app->currentScreen->onEnter) app->currentScreen->onEnter();
}

}  // namespace scene
