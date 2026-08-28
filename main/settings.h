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
