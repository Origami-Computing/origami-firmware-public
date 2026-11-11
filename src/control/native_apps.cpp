#include "control/native_apps.h"
#ifdef ORIGAMI_NATIVE_APPS
#include "services/native_apps.h"
#include "app.h"
#include "scene/screen.h"
#include "nav_handler.h"
namespace control { namespace native_apps {
namespace {
void (*originalExit)()=nullptr;
void exit(){
    if(originalExit)originalExit();
    originalExit=nullptr;
    services::native_apps::retire();
    // Never free here: the app may have called NavHandler::pop from its own
    // input callback. Both that code and exitApp's descriptor remain borrowed.
}
}
bool openPrepared(){
    void* descriptor=nullptr;
    if(!services::native_apps::activate(descriptor))return false;
    auto* app=static_cast<App*>(descriptor);
    if(!app->name||!app->initialScreen){services::native_apps::retire();return false;}
    originalExit=app->onExit;app->onExit=exit;
    if(!NavHandler::instance().open(app)){
        app->onExit=originalExit;originalExit=nullptr;services::native_apps::retire();return false;
    }
    return true;
}
void collect(){services::native_apps::collect();}
} }
#else
namespace control { namespace native_apps {bool openPrepared(){return false;}void collect(){}} }
#endif
