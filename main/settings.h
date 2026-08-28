#pragma once

// Runtime configuration in NVS. Upstream ESPAltherma keeps all of this in
// src/setup.h and needs a reflash to change it; here the MQTT broker, username
// and password are editable from the Config tab of the web UI and survive a
// reboot.
//
// secrets.h still supplies the first-boot defaults, so a freshly flashed board
// comes up already pointing at the right broker. Once saved from the web UI,
// NVS wins and secrets.h is ignored.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define ALT_SETTING_MAX 128

// Loads settings from NVS, falling back to the secrets.h defaults.
esp_err_t alt_settings_init(void);

const char *alt_settings_mqtt_uri(void);
const char *alt_settings_mqtt_user(void);
const char *alt_settings_mqtt_pass(void);

// True when a non-empty password is stored. The password itself is never sent
// to the browser.
bool alt_settings_mqtt_pass_set(void);

// Persists all three. Pass NULL for the password to keep the stored one, which
// is what the web UI does when the field is left blank.
esp_err_t alt_settings_set_mqtt(const char *uri, const char *user, const char *pass);

// ---- WiFi ---------------------------------------------------------------
// Credentials and IP mode live in NVS. secrets.h only seeds them on a device
// that has never been configured; with both empty the firmware brings up a
// SoftAP instead so the network can be chosen from a browser.

#define ALT_AP_SSID "AlthermaInterface"
#define ALT_AP_IP   "192.168.4.1"

const char *alt_settings_wifi_ssid(void);
const char *alt_settings_wifi_pass(void);

// True when a static address is configured; false means DHCP.
bool alt_settings_ip_static(void);
const char *alt_settings_ip_addr(void);
const char *alt_settings_ip_gw(void);
const char *alt_settings_ip_mask(void);
const char *alt_settings_ip_dns(void);

// True when no SSID is configured at all - the firmware then starts the
// provisioning SoftAP rather than trying to associate with nothing.
bool alt_settings_wifi_unconfigured(void);

// Pass NULL for the password to keep the stored one.
esp_err_t alt_settings_set_wifi(const char *ssid, const char *pass);
esp_err_t alt_settings_set_ip(bool use_static, const char *addr, const char *gw,
                              const char *mask, const char *dns);

// ---- GitHub releases ----------------------------------------------------
// owner/name of the repository whose releases the device may download and
// flash. The OTA path is locked to this repo, so the device can only ever
// install this project's own releases, never an arbitrary binary.
const char *alt_settings_gh_repo(void);
esp_err_t alt_settings_set_gh_repo(const char *repo);

// ---- X10A pin map -------------------------------------------------------
// Defaults come from board_config.h; NVS overrides once saved from the Config
// tab. Getting these wrong costs the heat pump link but not the device: WiFi
// and the web UI come up regardless, so a bad pin is fixable from the browser.

int alt_settings_rx_pin(void);
int alt_settings_tx_pin(void);

// Returns NULL if the pair is usable, otherwise a short reason. Rejects pins
// outside 0-39, the two being equal, GPIO6-11 (wired to the SPI flash - using
// one of those does not merely fail, it stops the chip booting) and TX on
// GPIO34-39 (input-only on the ESP32, so they cannot drive a UART TX).
const char *alt_settings_check_pins(int rx, int tx);

// Persists the pin map. Validates first and returns ESP_ERR_INVALID_ARG if
// alt_settings_check_pins() rejects the pair.
esp_err_t alt_settings_set_pins(int rx, int tx);
