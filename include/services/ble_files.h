#pragma once
#include "hal/hal_ble.h"

namespace services { namespace ble_files {
// Transfer queue, SD file and protocol lifetime. Bind at boot before radio tasks.
extern const hal::ble::FileReceiver receiver;
} }
