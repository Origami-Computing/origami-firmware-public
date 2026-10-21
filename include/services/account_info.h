#pragma once
namespace services { namespace account_info {
// Owned UI snapshot; a backend/auth revision change immediately hides old data.
struct Snapshot { char name[96]; };
Snapshot snapshot();
// Called only by the existing periodic network worker, under its reservation.
void poll();
}}
