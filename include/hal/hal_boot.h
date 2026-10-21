#pragma once

#include <stdbool.h>

namespace hal {
namespace boot {

enum class App {
  Menu,
  Mp3
};

bool setBootApp(App app);
void rebootTo(App app);

}  // namespace boot
}  // namespace hal
