#pragma once
#include <stddef.h>
#include <stdint.h>
namespace services { namespace app_http {
constexpr size_t kMaxBody=8192,kMaxRequest=4096,kMaxHeaders=8;
struct Header { char name[48]={},value[192]={}; };
struct Request {char url[768]={},method[16]="GET";Header headers[kMaxHeaders];size_t headerCount=0,bodySize=0;char body[kMaxRequest]={};uint32_t timeoutMs=15000;};
struct Response {int status=0;size_t size=0;char body[kMaxBody+1]={};};
// UI task only. Copies request; replaces the previous request. Returns 0 on
// invalid input/allocation failure. No I/O. Never inherits device credentials.
uint32_t request(const Request& request);
void stop(uint32_t token); // Stale owners cannot stop a replacement.
bool ready(uint32_t token); // Expires queued/offline requests too.
bool take(uint32_t token,Response& response); // Owned, single-consumption copy.
bool isActive();
// Remains owned between responses until cancel/VM close. Bulk sync must not
// slip between chained foreground requests and consume their deadline.
bool hasOwner();
bool pollDue();
// Existing WidgetPoll only, under its normal network/transport admission.
// Cooperative pause leaves work retryable until its original deadline.
void poll(bool (*cancelled)());
}}
