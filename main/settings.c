// Runtime configuration in NVS. Replaces the compile-time half of upstream
// ESPAltherma's src/setup.h for the MQTT connection.

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "secrets.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "altherma"
#define KEY_MQTT_URI  "mqtt_uri"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PASS "mqtt_pass"
#define KEY_RX_PIN    "rx_pin"
#define KEY_TX_PIN    "tx_pin"
#define KEY_GH_REPO   "gh_repo"

static char s_uri[ALT_SETTING_MAX];
static char s_user[ALT_SETTING_MAX];
static char s_pass[ALT_SETTING_MAX];
static int  s_rx_pin = ALT_UART_RX_PIN;
static int  s_tx_pin = ALT_UART_TX_PIN;
// owner/name of the GitHub repo whose releases the device may flash from.
static char s_gh_repo[ALT_SETTING_MAX] = ALT_GITHUB_REPO_DEFAULT;

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
    load_str(h, KEY_GH_REPO,   s_gh_repo, sizeof(s_gh_repo));

    int32_t v = 0;
    if (nvs_get_i32(h, KEY_RX_PIN, &v) == ESP_OK) s_rx_pin = (int)v;
    if (nvs_get_i32(h, KEY_TX_PIN, &v) == ESP_OK) s_tx_pin = (int)v;

    // A stored pair that somehow fails validation is discarded rather than
    // used: booting with GPIO6-11 as a UART pin would not come back.
    const char *bad = alt_settings_check_pins(s_rx_pin, s_tx_pin);
    if (bad) {
        ESP_LOGE(TAG, "stored pin map rejected (%s); falling back to RX=%d TX=%d",
                 bad, ALT_UART_RX_PIN, ALT_UART_TX_PIN);
        s_rx_pin = ALT_UART_RX_PIN;
        s_tx_pin = ALT_UART_TX_PIN;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "broker %s, user \"%s\", password %s, X10A RX=GPIO%d TX=GPIO%d",
             s_uri, s_user, s_pass[0] ? "set" : "empty", s_rx_pin, s_tx_pin);
    return ESP_OK;
}

const char *alt_settings_gh_repo(void) { return s_gh_repo; }

esp_err_t alt_settings_set_gh_repo(const char *repo)
{
    if (!repo) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gh_repo, repo, sizeof(s_gh_repo));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_GH_REPO, s_gh_repo);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

int alt_settings_rx_pin(void) { return s_rx_pin; }
int alt_settings_tx_pin(void) { return s_tx_pin; }

const char *alt_settings_check_pins(int rx, int tx)
{
    if (rx < 0 || rx > 39 || tx < 0 || tx > 39) {
        return "pins must be 0-39";
    }
    if (rx == tx) {
        return "RX and TX must differ";
    }
    // GPIO6-11 are the SPI flash bus. Handing one to the UART does not just
    // fail to work, it stops the chip booting - and this device has no USB
    // attached, so that would mean opening the heat pump to recover it.
    if ((rx >= 6 && rx <= 11) || (tx >= 6 && tx <= 11)) {
        return "GPIO6-11 are wired to the SPI flash";
    }
    // 34-39 have no output driver on the ESP32.
    if (tx >= 34) {
        return "GPIO34-39 are input-only, cannot be TX";
    }
    return NULL;
}

esp_err_t alt_settings_set_pins(int rx, int tx)
{
    if (alt_settings_check_pins(rx, tx) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_rx_pin = rx;
    s_tx_pin = tx;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, KEY_RX_PIN, (int32_t)rx);
    if (err == ESP_OK) err = nvs_set_i32(h, KEY_TX_PIN, (int32_t)tx);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved X10A pins RX=GPIO%d TX=GPIO%d", rx, tx);
    }
    return err;
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
