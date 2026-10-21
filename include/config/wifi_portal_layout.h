#pragma once

#include <stdint.h>

#include "config/ui_theme.h"

namespace wifi_portal_layout {

// BEGIN apply_ui_source_edits.py wifi-portal/portal
static constexpr int16_t kContentBgX = 0;
static constexpr int16_t kContentBgY = 30;
static constexpr int16_t kContentBgW = 280;
static constexpr int16_t kContentBgH = 210;
static constexpr uint8_t kContentBgRadius = 0;
static constexpr uint8_t kContentBgZ = 0;
static constexpr bool kContentBgVisible = true;
static constexpr uint16_t kContentBgFillColor = kUiBg;

static constexpr int16_t kWifiQrX = 14;
static constexpr int16_t kWifiQrY = 77;
static constexpr int16_t kWifiQrW = 116;
static constexpr int16_t kWifiQrH = 116;
static constexpr uint8_t kWifiQrModuleSize = 4;
static constexpr uint8_t kWifiQrZ = 1;
static constexpr bool kWifiQrVisible = true;
static constexpr uint16_t kWifiQrFgColor = kUiFg;
static constexpr uint16_t kWifiQrBgColor = kUiBg;

static constexpr int16_t kWifiTitleX = 142;
static constexpr int16_t kWifiTitleY = 36;
static constexpr int16_t kWifiTitleW = 110;
static constexpr int16_t kWifiTitleH = 10;
static constexpr uint8_t kWifiTitleTextSize = 1;
static constexpr uint8_t kWifiTitleZ = 1;
static constexpr bool kWifiTitleVisible = true;
static constexpr uint16_t kWifiTitleFgColor = kUiAccent;
static constexpr uint16_t kWifiTitleBgColor = kUiBg;
static constexpr char kWifiTitleText[] = "WiFi Portal";

static constexpr int16_t kWifiScanX = 142;
static constexpr int16_t kWifiScanY = 52;
static constexpr int16_t kWifiScanW = 110;
static constexpr int16_t kWifiScanH = 10;
static constexpr uint8_t kWifiScanTextSize = 1;
static constexpr uint8_t kWifiScanZ = 1;
static constexpr bool kWifiScanVisible = true;
static constexpr uint16_t kWifiScanFgColor = kUiFg;
static constexpr uint16_t kWifiScanBgColor = kUiBg;
static constexpr char kWifiScanText[] = "Scan QR or join";

static constexpr int16_t kWifiManualX = 142;
static constexpr int16_t kWifiManualY = 64;
static constexpr int16_t kWifiManualW = 110;
static constexpr int16_t kWifiManualH = 10;
static constexpr uint8_t kWifiManualTextSize = 1;
static constexpr uint8_t kWifiManualZ = 1;
static constexpr bool kWifiManualVisible = true;
static constexpr uint16_t kWifiManualFgColor = kUiFg;
static constexpr uint16_t kWifiManualBgColor = kUiBg;
static constexpr char kWifiManualText[] = "AP manually:";

static constexpr int16_t kWifiSsidX = 142;
static constexpr int16_t kWifiSsidY = 82;
static constexpr int16_t kWifiSsidW = 110;
static constexpr int16_t kWifiSsidH = 10;
static constexpr uint8_t kWifiSsidTextSize = 1;
static constexpr uint8_t kWifiSsidZ = 1;
static constexpr bool kWifiSsidVisible = true;
static constexpr uint16_t kWifiSsidFgColor = kUiFg;
static constexpr uint16_t kWifiSsidBgColor = kUiBg;

static constexpr int16_t kWifiPasswordX = 142;
static constexpr int16_t kWifiPasswordY = 94;
static constexpr int16_t kWifiPasswordW = 110;
static constexpr int16_t kWifiPasswordH = 10;
static constexpr uint8_t kWifiPasswordTextSize = 1;
static constexpr uint8_t kWifiPasswordZ = 1;
static constexpr bool kWifiPasswordVisible = true;
static constexpr uint16_t kWifiPasswordFgColor = kUiMuted;
static constexpr uint16_t kWifiPasswordBgColor = kUiBg;

static constexpr int16_t kWifiIpX = 142;
static constexpr int16_t kWifiIpY = 114;
static constexpr int16_t kWifiIpW = 110;
static constexpr int16_t kWifiIpH = 10;
static constexpr uint8_t kWifiIpTextSize = 1;
static constexpr uint8_t kWifiIpZ = 1;
static constexpr bool kWifiIpVisible = true;
static constexpr uint16_t kWifiIpFgColor = 0x07E0;
static constexpr uint16_t kWifiIpBgColor = kUiBg;
static constexpr char kWifiIpText[] = "192.168.4.1";

static constexpr int16_t kWifiStaX = 142;
static constexpr int16_t kWifiStaY = 134;
static constexpr int16_t kWifiStaW = 110;
static constexpr int16_t kWifiStaH = 10;
static constexpr uint8_t kWifiStaTextSize = 1;
static constexpr uint8_t kWifiStaZ = 1;
static constexpr bool kWifiStaVisible = true;
static constexpr uint16_t kWifiStaFgColor = kUiMuted;
static constexpr uint16_t kWifiStaOkColor = 0x07E0;
static constexpr uint16_t kWifiStaBgColor = kUiBg;

static constexpr int16_t kWifiStaIpX = 142;
static constexpr int16_t kWifiStaIpY = 146;
static constexpr int16_t kWifiStaIpW = 110;
static constexpr int16_t kWifiStaIpH = 10;
static constexpr uint8_t kWifiStaIpTextSize = 1;
static constexpr uint8_t kWifiStaIpZ = 1;
static constexpr bool kWifiStaIpVisible = true;
static constexpr uint16_t kWifiStaIpFgColor = kUiFg;
static constexpr uint16_t kWifiStaIpBgColor = kUiBg;

static constexpr int16_t kWifiClientsX = 142;
static constexpr int16_t kWifiClientsY = 166;
static constexpr int16_t kWifiClientsW = 110;
static constexpr int16_t kWifiClientsH = 10;
static constexpr uint8_t kWifiClientsTextSize = 1;
static constexpr uint8_t kWifiClientsZ = 1;
static constexpr bool kWifiClientsVisible = true;
static constexpr uint16_t kWifiClientsFgColor = kUiFg;
static constexpr uint16_t kWifiClientsBgColor = kUiBg;

static constexpr int16_t kWifiFooterBgX = 0;
static constexpr int16_t kWifiFooterBgY = 218;
static constexpr int16_t kWifiFooterBgW = 280;
static constexpr int16_t kWifiFooterBgH = 22;
static constexpr uint8_t kWifiFooterBgRadius = 0;
static constexpr uint8_t kWifiFooterBgZ = 1;
static constexpr bool kWifiFooterBgVisible = true;
static constexpr uint16_t kWifiFooterBgFillColor = kUiBg;

static constexpr int16_t kWifiFooterX = 8;
static constexpr int16_t kWifiFooterY = 223;
static constexpr int16_t kWifiFooterW = 264;
static constexpr int16_t kWifiFooterH = 22;
static constexpr uint8_t kWifiFooterTextSize = 1;
static constexpr uint8_t kWifiFooterZ = 2;
static constexpr bool kWifiFooterVisible = true;
static constexpr uint16_t kWifiFooterFgColor = kUiMuted;
static constexpr uint16_t kWifiFooterBgColor = kUiBg;
static constexpr char kWifiFooterText[] = "LB: Back";// END apply_ui_source_edits.py wifi-portal/portal

}  // namespace wifi_portal_layout
