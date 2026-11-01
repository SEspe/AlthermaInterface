#include "external.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mqtt.h"
#include "settings.h"

static const char *TAG = "external";

typedef struct {
    char label[ALT_EXT_LABEL_MAX];
    char topic[ALT_EXT_TOPIC_MAX];
    char unit[ALT_EXT_UNIT_MAX];
    char value[ALT_EXT_VALUE_MAX];
    int64_t at_ms; // 0 until the first message arrives
} ext_entry_t;

static ext_entry_t s_entries[ALT_EXT_MAX];
static int s_count;

// Written by the MQTT task, read by the web task. A mutex, not a spinlock: the
// JSON is formatted while holding it.
static SemaphoreHandle_t s_mutex;

// Characters that would break the JSON or the Config tab's text format.
static bool plain_char(unsigned char c)
{
    return c >= 0x20 && c != '"' && c != '\\' && c != '|' && c != ';' && c != 0x7f;
}

static bool plain_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!plain_char((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

// Splits one "label|topic|unit" record. Returns false if the shape is wrong.
static bool split_record(const char *rec, size_t n, const char **label, size_t *llen,
                         const char **topic, size_t *tlen, const char **unit, size_t *ulen)
{
    const char *p1 = memchr(rec, '|', n);
    if (!p1) {
        return false;
    }
    const char *end = rec + n;
    const char *p2 = memchr(p1 + 1, '|', (size_t)(end - (p1 + 1)));
    if (!p2) {
        return false;
    }
    *label = rec;
    *llen = (size_t)(p1 - rec);
    *topic = p1 + 1;
    *tlen = (size_t)(p2 - (p1 + 1));
    *unit = p2 + 1;
    *ulen = (size_t)(end - (p2 + 1));
    return true;
}

bool alt_ext_check(const char *text, char *err, size_t errlen)
{
    int records = 0;
    const char *p = text;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        if (n > 0) {
            const char *label, *topic, *unit;
            size_t llen, tlen, ulen;
            if (!split_record(p, n, &label, &llen, &topic, &tlen, &unit, &ulen)) {
                snprintf(err, errlen, "each entry needs label|topic|unit");
                return false;
            }
            if (llen == 0 || llen >= ALT_EXT_LABEL_MAX || !plain_text(label, llen)) {
                snprintf(err, errlen, "label must be 1-%d plain characters", ALT_EXT_LABEL_MAX - 1);
                return false;
            }
            if (tlen == 0 || tlen >= ALT_EXT_TOPIC_MAX || !plain_text(topic, tlen)
                || memchr(topic, '+', tlen) || memchr(topic, '#', tlen)) {
                snprintf(err, errlen, "topic must be exact (no + or #), 1-%d characters", ALT_EXT_TOPIC_MAX - 1);
                return false;
            }
            if (ulen >= ALT_EXT_UNIT_MAX || !plain_text(unit, ulen)) {
                snprintf(err, errlen, "unit must be up to %d plain characters", ALT_EXT_UNIT_MAX - 1);
                return false;
            }
            records++;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    if (records > ALT_EXT_MAX) {
        snprintf(err, errlen, "at most %d external sensors", ALT_EXT_MAX);
        return false;
    }
    return true;
}

esp_err_t alt_ext_set(const char *text)
{
    char err[64];
    if (!alt_ext_check(text, err, sizeof(err))) {
        ESP_LOGW(TAG, "rejected external sensor list: %s", err);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = alt_settings_set_ext_topics(text);
    if (e != ESP_OK) {
        return e;
    }
    alt_ext_load();
    alt_ext_subscribe_all();
    return ESP_OK;
}

void alt_ext_load(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "cannot create mutex");
            return;
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;

    const char *p = alt_settings_ext_topics();
    while (*p && s_count < ALT_EXT_MAX) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        if (n > 0) {
            const char *label, *topic, *unit;
            size_t llen, tlen, ulen;
            if (split_record(p, n, &label, &llen, &topic, &tlen, &unit, &ulen)) {
                ext_entry_t *e = &s_entries[s_count++];
                snprintf(e->label, sizeof(e->label), "%.*s", (int)llen, label);
                snprintf(e->topic, sizeof(e->topic), "%.*s", (int)tlen, topic);
                snprintf(e->unit, sizeof(e->unit), "%.*s", (int)ulen, unit);
            }
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "%d external topic(s) configured", s_count);
}

void alt_ext_subscribe_all(void)
{
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (!alt_mqtt_subscribe(s_entries[i].topic)) {
            ESP_LOGW(TAG, "subscribe failed or not connected: %s", s_entries[i].topic);
        }
    }
    xSemaphoreGive(s_mutex);
}

// The payload as text, trimmed and made safe for the JSON. ESPEasy sends plain
// numbers; anything else is shown as received, minus the characters that would
// need escaping.
static void copy_value(char *out, size_t outlen, const char *data, size_t dlen)
{
    while (dlen > 0 && isspace((unsigned char)*data)) {
        data++;
        dlen--;
    }
    while (dlen > 0 && isspace((unsigned char)data[dlen - 1])) {
        dlen--;
    }
    size_t j = 0;
    for (size_t i = 0; i < dlen && j + 1 < outlen; i++) {
        unsigned char c = (unsigned char)data[i];
        out[j++] = plain_char(c) ? (char)c : '?';
    }
    out[j] = '\0';
}

void alt_ext_on_data(const char *topic, size_t tlen, const char *data, size_t dlen)
{
    if (!s_mutex) {
        return;
    }
    char value[ALT_EXT_VALUE_MAX];
    copy_value(value, sizeof(value), data, dlen);
    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strlen(s_entries[i].topic) == tlen && memcmp(s_entries[i].topic, topic, tlen) == 0) {
            strlcpy(s_entries[i].value, value, sizeof(s_entries[i].value));
            s_entries[i].at_ms = now_ms;
        }
    }
    xSemaphoreGive(s_mutex);
}

size_t alt_ext_json(char *buf, size_t len)
{
    size_t n = 0;
    if (!s_mutex) {
        return (size_t)snprintf(buf, len, "{\"items\":[]}");
    }
    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    n += (size_t)snprintf(buf + n, len - n, "{\"items\":[");
    for (int i = 0; i < s_count && n < len; i++) {
        const ext_entry_t *e = &s_entries[i];
        long age = e->at_ms ? (long)((now_ms - e->at_ms) / 1000) : -1;
        n += (size_t)snprintf(buf + n, len - n,
                              "%s{\"label\":\"%s\",\"unit\":\"%s\",\"topic\":\"%s\","
                              "\"value\":\"%s\",\"age\":%ld}",
                              i ? "," : "", e->label, e->unit, e->topic, e->value, age);
    }
    if (n < len) {
        n += (size_t)snprintf(buf + n, len - n, "]}");
    }
    xSemaphoreGive(s_mutex);
    return n < len ? n : len - 1;
}
