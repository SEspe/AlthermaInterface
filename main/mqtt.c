// Ported from upstream ESPAltherma include/mqtt.h (MIT, (c) 2020 Raomin).
//
// The payload format is deliberately identical to upstream's: one JSON object
// per poll cycle on espaltherma/ATTR, numeric values unquoted and everything
// else quoted, with WifiRSSI and FreeMem appended. Home Assistant reads it
// through json_attributes_topic, so any change here breaks existing dashboards
// for no gain.

#include "mqtt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "converters.h"
#include "homeassistant.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "mqtt";

// Upstream MAX_MSG_SIZE. Far more than protocol S needs (25 labels), but the
// firmware should not silently truncate on a protocol-I machine either.
#define ALT_JSON_MAX 7120

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_json[ALT_JSON_MAX];

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "connected to %s", alt_settings_mqtt_uri());
        // Retained, so a subscriber that connects later still learns we are up.
        esp_mqtt_client_publish(s_client, ALT_MQTT_TOPIC_LWT, "Online", 0, 0, 1);

        // Retained discovery, republished on every connect: Home Assistant
        // rebuilds the entities from it after a restart of either end.
        size_t dlen = 0;
        const char *discovery = alt_ha_discovery_payload(&dlen);
        if (discovery && dlen > 0) {
            int msg_id = esp_mqtt_client_publish(s_client, ALT_HA_DISCOVERY_TOPIC,
                                                 discovery, (int)dlen, 0, 1);
            if (msg_id < 0) {
                ESP_LOGE(TAG, "discovery publish failed (%u bytes) - is the "
                              "MQTT buffer large enough?", (unsigned)dlen);
            } else {
                ESP_LOGI(TAG, "published HA discovery, %u bytes", (unsigned)dlen);
            }
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;

    case MQTT_EVENT_ERROR:
        if (e && e->error_handle) {
            ESP_LOGW(TAG, "error: type %d, transport sock errno %d",
                     e->error_handle->error_type,
                     e->error_handle->esp_transport_sock_errno);
        }
        break;

    default:
        break;
    }
}

esp_err_t alt_mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {0};
    // Broker details come from NVS (Config tab), not from secrets.h, once the
    // user has saved them once.
    cfg.broker.address.uri = alt_settings_mqtt_uri();
    cfg.session.last_will.topic = ALT_MQTT_TOPIC_LWT;
    cfg.session.last_will.msg = "Offline";
    cfg.session.last_will.qos = 0;
    cfg.session.last_will.retain = 1;
    // Must hold the largest single message, which is the retained HA discovery
    // payload (~7 KB for this definition), not the ATTR payload.
    cfg.buffer.size = 16384;

    if (strlen(alt_settings_mqtt_user()) > 0) {
        cfg.credentials.username = alt_settings_mqtt_user();
        cfg.credentials.authentication.password = alt_settings_mqtt_pass();
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   &on_mqtt_event, NULL));
    ESP_LOGI(TAG, "connecting to %s", alt_settings_mqtt_uri());
    return esp_mqtt_client_start(s_client);
}

bool alt_mqtt_is_connected(void)
{
    return s_connected;
}

// Upstream's test: a value is emitted unquoted only if every character is a
// digit, a dot, or a leading minus. Anything else - "ON", "R410A", "---",
// "Heating", "Conv 123 not avail." - gets quoted.
static bool is_numeric(const char *s)
{
    if (!s || s[0] == '\0') {
        return false;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (!isdigit((unsigned char)c) && c != '.' && !(c == '-' && i == 0)) {
            return false;
        }
    }
    return true;
}

esp_err_t alt_mqtt_publish_values(void)
{
    if (!s_connected) {
        ESP_LOGW(TAG, "not connected, dropping this cycle's values");
        return ESP_ERR_INVALID_STATE;
    }

    size_t pos = 0;
    s_json[0] = '\0';
    pos += snprintf(s_json + pos, ALT_JSON_MAX - pos, "{");

    size_t total = converter_label_count();
    for (size_t i = 0; i < total; i++) {
        const char *label;
        const char *value;
        if (!converter_label_at(i, NULL, &label, &value)) {
            continue;
        }
        // A label never read this cycle still holds its previous value, exactly
        // as upstream behaves; an empty one has never been read at all.
        if (value[0] == '\0') {
            continue;
        }
        if (is_numeric(value)) {
            pos += snprintf(s_json + pos, ALT_JSON_MAX - pos, "\"%s\":%s,", label, value);
        } else {
            pos += snprintf(s_json + pos, ALT_JSON_MAX - pos, "\"%s\":\"%s\",", label, value);
        }
        if (pos >= ALT_JSON_MAX) {
            ESP_LOGE(TAG, "payload truncated at label %u", (unsigned)i);
            return ESP_ERR_NO_MEM;
        }
    }

    pos += snprintf(s_json + pos, ALT_JSON_MAX - pos, "\"WifiRSSI\":\"%ddBm\",", alt_wifi_rssi());
    pos += snprintf(s_json + pos, ALT_JSON_MAX - pos, "\"FreeMem\":\"%u\",",
                    (unsigned)esp_get_free_heap_size());

    // Overwrite the trailing comma, as upstream does.
    if (pos > 1 && s_json[pos - 1] == ',') {
        s_json[pos - 1] = '}';
    } else {
        pos += snprintf(s_json + pos, ALT_JSON_MAX - pos, "}");
    }

    int msg = esp_mqtt_client_publish(s_client, ALT_MQTT_TOPIC_ATTR, s_json, 0, 0, 0);
    if (msg < 0) {
        ESP_LOGE(TAG, "publish failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published %u bytes to %s", (unsigned)strlen(s_json), ALT_MQTT_TOPIC_ATTR);
    return ESP_OK;
}

// ---------------------------------------------------------------- log mirror

static vprintf_like_t s_prev_vprintf;
static volatile bool s_in_hook;

// Upstream's MQTTSerial mirrors every log line to espaltherma/log. The hazard
// that does not exist in Arduino: esp-mqtt logs from its own task, so a publish
// made from inside the log hook can re-enter the hook. The flag below breaks
// that recursion - a dropped log line is a much better outcome than a deadlock
// in the client task.
static int log_vprintf(const char *fmt, va_list args)
{
    // Copy the arguments BEFORE handing them to the previous handler: a
    // va_list is consumed by the call, and reusing it afterwards is undefined.
    va_list copy;
    va_copy(copy, args);

    int written = s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;

    if (s_in_hook || !s_connected || !s_client) {
        va_end(copy);
        return written;
    }
    s_in_hook = true;

    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0) {
        esp_mqtt_client_publish(s_client, ALT_MQTT_TOPIC_LOG, line, 0, 0, 0);
    }

    s_in_hook = false;
    return written;
}

void alt_mqtt_log_redirect(void)
{
    s_prev_vprintf = esp_log_set_vprintf(&log_vprintf);
    ESP_LOGI(TAG, "log mirror active on %s", ALT_MQTT_TOPIC_LOG);
}
