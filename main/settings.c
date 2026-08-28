// Runtime configuration in NVS. Replaces the compile-time half of upstream
// ESPAltherma's src/setup.h for the MQTT connection.

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "secrets.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "altherma"
#define KEY_MQTT_URI  "mqtt_uri"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PASS "mqtt_pass"

static char s_uri[ALT_SETTING_MAX];
static char s_user[ALT_SETTING_MAX];
static char s_pass[ALT_SETTING_MAX];

// Reads one string key, leaving the caller's default in place if it is absent.
static void load_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t got = len;
    esp_err_t err = nvs_get_str(h, key, out, &got);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read %s: %s", key, esp_err_to_name(err));
    }
}

esp_err_t alt_settings_init(void)
{
    // First-boot defaults from secrets.h; NVS overrides them if present.
    strlcpy(s_uri,  ALT_MQTT_URI,      sizeof(s_uri));
    strlcpy(s_user, ALT_MQTT_USERNAME, sizeof(s_user));
    strlcpy(s_pass, ALT_MQTT_PASSWORD, sizeof(s_pass));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using secrets.h defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s - using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    load_str(h, KEY_MQTT_URI,  s_uri,  sizeof(s_uri));
    load_str(h, KEY_MQTT_USER, s_user, sizeof(s_user));
    load_str(h, KEY_MQTT_PASS, s_pass, sizeof(s_pass));
    nvs_close(h);

    ESP_LOGI(TAG, "broker %s, user \"%s\", password %s",
             s_uri, s_user, s_pass[0] ? "set" : "empty");
    return ESP_OK;
}

const char *alt_settings_mqtt_uri(void)  { return s_uri; }
const char *alt_settings_mqtt_user(void) { return s_user; }
const char *alt_settings_mqtt_pass(void) { return s_pass; }

bool alt_settings_mqtt_pass_set(void) { return s_pass[0] != '\0'; }

esp_err_t alt_settings_set_mqtt(const char *uri, const char *user, const char *pass)
{
    if (uri)  strlcpy(s_uri,  uri,  sizeof(s_uri));
    if (user) strlcpy(s_user, user, sizeof(s_user));
    // NULL means "leave the stored password alone" - the web UI sends a blank
    // field when the user does not want to change it.
    if (pass) strlcpy(s_pass, pass, sizeof(s_pass));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, KEY_MQTT_URI, s_uri);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_USER, s_user);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_PASS, s_pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "saved broker %s, user \"%s\"", s_uri, s_user);
    return ESP_OK;
}
