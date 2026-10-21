#pragma once
#include "core/wifi_qr_parser.h"

namespace services { namespace wifi_provision {
using core::wifi_qr::Credentials;

/**
 * Persist credentials to NVS and start a non-blocking STA join.
 *
 * @param keepApUp  When true (web-portal context), join in APSTA mode so the
 *                  soft-AP stays up — otherwise tearing it down here kills the
 *                  phone's TCP socket before the HTTP response can be sent, and
 *                  the page shows "Request failed" even though creds were saved.
 *                  When false (camera-scan context, no AP running), switch to
 *                  STA-only.
 */
bool saveAndConnect(const Credentials& creds, bool keepApUp = false);

/**
 * Connect to an already-saved network by SSID. Promotes it to the front of the
 * join list, then kicks off the deferred radio bring-up + STA join on the
 * wifi_mgr task (Core 0) via hal::wifi::requestConnect(). This is the single
 * connect path shared by the Saved-Networks "Connect" button and any other
 * caller — the actual WiFi.mode()/begin() calls must run on Core 0 or they fail
 * (esp_wifi_init returns 257 off-core). Returns false if ssid is empty/oversized.
 */
bool connectSaved(const char* ssid);

/** Remove the selected saved entry and, on reported success, ask wifi_mgr to
 * reconcile its credential cache/radio state. UI-task operation; never performs
 * a radio mode change here. Preserves the existing storage result: false may
 * follow RAM mutation, and true does not certify every NVS key deletion.
 * Callers must re-read the list rather than assuming failure left it unchanged.
 */
bool forgetSaved(int index);

} }
