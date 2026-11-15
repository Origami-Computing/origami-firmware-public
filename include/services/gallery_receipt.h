#pragma once
#include <stdint.h>
namespace services { namespace gallery_receipt {
// Receipts are scoped to the local original and the account used to upload it.
bool confirmed(const char* path);
void remember(const char* path, const char* account, uint32_t bytes, uint32_t modified);
} }
