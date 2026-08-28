#pragma once

// On-device web UI. Not part of upstream ESPAltherma, which is configured by
// editing src/setup.h and reflashing.
//
// Four tabs, served from the device itself:
//   Daikin Data - every decoded label and its current value
//   WiFi        - association diagnostics
//   Config      - MQTT broker, username, password (saved to NVS)
//   OTA         - firmware upload
//
// Endpoints:
//   GET  /             the page
//   GET  /api/status   device, WiFi and MQTT state
//   GET  /api/values   decoded heat pump values
//   GET  /api/config   MQTT settings (never the password)
//   POST /api/config   save MQTT settings, then reboot
//   POST /ota/upload   raw firmware image, then reboot

#include "esp_err.h"

esp_err_t alt_web_start(void);
