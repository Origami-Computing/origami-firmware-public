#pragma once
#include "core/manufacturing_access.h"
namespace services { namespace manufacturing_access {
// UI-task only. Shared by USB serial admission and shipping teardown.
core::ManufacturingAccess& session();
} }
