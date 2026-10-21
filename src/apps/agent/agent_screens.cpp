#include "apps/agent/agent_screens.h"

namespace apps {
namespace agent {

const ::scene::Screen& appScreenFor(Stage stage) {
    switch (stage) {
        case Stage::Eyes:      return kScreenEyes;
        case Stage::Recording: return kScreenRecording;
        case Stage::Thinking:  return kScreenThinking;
        case Stage::Reply:     return kScreenReply;
        case Stage::Error:     return kScreenError;
    }
    return kScreenEyes;  // unreachable; defensive default
}

}  // namespace agent
}  // namespace apps
