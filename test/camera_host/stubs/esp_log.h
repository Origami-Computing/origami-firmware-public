#pragma once
#include "camera_test_sdk.h"
#define ESP_LOGW(tag, ...) do { char message[160]; snprintf(message, sizeof(message), __VA_ARGS__); fake.logs.push_back(message); } while (0)
