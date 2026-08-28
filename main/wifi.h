#pragma once

// WiFi station. Ported from the WiFi handling in upstream ESPAltherma's
// src/main.cpp (MIT, (c) 2020 Raomin): esp_wifi + an event group replace
// WiFi.begin()/WiFi.status() polling, and the reconnect ladder is kept.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Brings up station mode and starts connecting. Returns as soon as the
// connection attempt is under way - it does not block until associated.
esp_err_t alt_wifi_start(void);

// True once associated with an IP address.
bool alt_wifi_is_connected(void);

// Blocks until connected or the timeout expires. Returns the connected state.
bool alt_wifi_wait_connected(uint32_t timeout_ms);

// Current RSSI in dBm, or 0 when not connected. Published as WifiRSSI.
int alt_wifi_rssi(void);

// Dotted-quad IP into out, or "0.0.0.0" when not connected.
void alt_wifi_ip(char *out, size_t len);

// The configured SSID.
const char *alt_wifi_ssid(void);

// Channel and BSSID of the AP we are associated with, for the diagnostics tab.
int alt_wifi_channel(void);
void alt_wifi_bssid(char *out, size_t len);

// True when running the provisioning SoftAP instead of joining a network,
// which happens when no SSID is configured.
bool alt_wifi_ap_mode(void);

// Scans for networks and writes a JSON array to `out`:
//   [{"ssid":"...","rssi":-60,"ch":6,"open":false}, ...]
// sorted strongest first, duplicates removed. Blocks for a few seconds.
// Returns the number of entries written.
int alt_wifi_scan_json(char *out, size_t len);
