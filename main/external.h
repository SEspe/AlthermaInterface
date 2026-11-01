#pragma once

// External sensors: values this unit does not read from the heat pump, but
// that other devices publish on the same MQTT broker (ESPEasy nodes, a
// PowerMeter, and so on). The device subscribes to the topics listed on the
// Config tab, keeps the latest value of each, and shows them on the Home page
// in a separate, shaded block, so nothing from outside the heat pump is mixed
// into its own readings.
//
// The list is text on the Config tab: one entry per line as
//   label|topic|unit
// The stored form uses ';' between entries. Topics are matched exactly, so no
// MQTT wildcards are allowed.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define ALT_EXT_MAX        16
#define ALT_EXT_LABEL_MAX  32
#define ALT_EXT_TOPIC_MAX  128
#define ALT_EXT_UNIT_MAX   8
#define ALT_EXT_VALUE_MAX  24

// Checks the Config tab text. Returns true when every entry is well formed; on
// failure writes a short reason into err.
bool alt_ext_check(const char *text, char *err, size_t errlen);

// Validates, saves to NVS, rebuilds the table and subscribes on the live broker.
// ESP_ERR_INVALID_ARG when alt_ext_check() rejects the text.
esp_err_t alt_ext_set(const char *text);

// Rebuilds the table from the saved text. Called at start-up.
void alt_ext_load(void);

// Subscribes every table topic on the connected client. Called on each connect.
void alt_ext_subscribe_all(void);

// Called for every incoming MQTT message. Topics not in the table are ignored.
void alt_ext_on_data(const char *topic, size_t tlen, const char *data, size_t dlen);

// Writes {"items":[...]} for GET /api/external. Returns the length written.
size_t alt_ext_json(char *buf, size_t len);
