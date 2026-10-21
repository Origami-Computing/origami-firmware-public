#include "services/manufacturing_access.h"
namespace services { namespace manufacturing_access {
core::ManufacturingAccess& session() {
    static core::ManufacturingAccess access;
    return access;
}
} }
