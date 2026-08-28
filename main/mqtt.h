#pragma once

// MQTT publishing. Ported from upstream ESPAltherma's include/mqtt.h
// (MIT, (c) 2020 Raomin): PubSubClient becomes esp_mqtt_client.
//
// Topics are kept byte-identical to upstream so an existing Home Assistant
// setup, or a second unit still running upstream's firmware, needs no change.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ALT_MQTT_TOPIC_ATTR "espaltherma/ATTR"
#define ALT_MQTT_TOPIC_LWT  "espaltherma/LWT"
#define ALT_MQTT_TOPIC_LOG  "espaltherma/log"

// Starts the client. Non-blocking; it connects and reconnects on its own.
esp_err_t alt_mqtt_start(void);

bool alt_mqtt_is_connected(void);

// Builds one JSON object from every decoded label and publishes it to
// espaltherma/ATTR, in upstream's format. Call once per poll cycle, after all
// registries have been read.
esp_err_t alt_mqtt_publish_values(void);

// Broker activity counters for the Debug tab. Any pointer may be NULL.
void alt_mqtt_stats(uint32_t *ok, uint32_t *fail, uint32_t *connects,
                    uint32_t *disconnects, int64_t *last_pub_ms);

// Mirrors ESP_LOGx output to espaltherma/log, as upstream's MQTTSerial does.
// Call after alt_mqtt_start().
void alt_mqtt_log_redirect(void);
