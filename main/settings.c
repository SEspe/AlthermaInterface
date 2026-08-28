// Runtime configuration in NVS. Replaces the compile-time half of upstream
// ESPAltherma's src/setup.h for the MQTT connection.

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board_config.h"

// secrets.h is git-ignored and seeds first-boot defaults on a configured
// machine. A clean clone does not have it, and must still build - the device
// then comes up unconfigured and is set up through the web UI, which is the
// intended path for a new board anyway.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef ALT_MQTT_URI
#define ALT_MQTT_URI ""
#endif
#ifndef ALT_MQTT_USERNAME
#define ALT_MQTT_USERNAME ""
#endif
#ifndef ALT_MQTT_PASSWORD
#define ALT_MQTT_PASSWORD ""
#endif

static const char *TAG = "settings";

#define NVS_NAMESPACE "altherma"
#define KEY_MQTT_URI  "mqtt_uri"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PASS "mqtt_pass"
#define KEY_RX_PIN    "rx_pin"
#define KEY_TX_PIN    "tx_pin"
#define KEY_GH_REPO   "gh_repo"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_IP_MODE   "ip_mode"
#define KEY_IP_ADDR   "ip_addr"
#define KEY_IP_GW     "ip_gw"
#define KEY_IP_MASK   "ip_mask"
#define KEY_IP_DNS    "ip_dns"

static char s_uri[ALT_SETTING_MAX];
static char s_user[ALT_SETTING_MAX];
static char s_pass[ALT_SETTING_MAX];
static int  s_rx_pin = ALT_UART_RX_PIN;
static int  s_tx_pin = ALT_UART_TX_PIN;
// owner/name of the GitHub repo whose releases the device may flash from.
static char s_gh_repo[ALT_SETTING_MAX] = ALT_GITHUB_REPO_DEFAULT;

static char s_wifi_ssid[ALT_SETTING_MAX];
static char s_wifi_pass[ALT_SETTING_MAX];
static bool s_ip_static;
static char s_ip_addr[20];
static char s_ip_gw[20];
static char s_ip_mask[20] = "255.255.255.0";
static char s_ip_dns[20];

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

    // WiFi seeds from secrets.h the same way. An unconfigured build leaves
    // these empty, which is what puts the firmware into SoftAP provisioning.
#ifdef ALT_WIFI_SSID
    strlcpy(s_wifi_ssid, ALT_WIFI_SSID,     sizeof(s_wifi_ssid));
    strlcpy(s_wifi_pass, ALT_WIFI_PASSWORD, sizeof(s_wifi_pass));
#endif
#ifdef ALT_WIFI_STATIC_IP
    s_ip_static = true;
    strlcpy(s_ip_addr, ALT_WIFI_STATIC_IP, sizeof(s_ip_addr));
    strlcpy(s_ip_gw,   ALT_WIFI_GATEWAY,   sizeof(s_ip_gw));
    strlcpy(s_ip_mask, ALT_WIFI_NETMASK,   sizeof(s_ip_mask));
    strlcpy(s_ip_dns,  ALT_WIFI_DNS,       sizeof(s_ip_dns));
#endif

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
    load_str(h, KEY_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid));
    load_str(h, KEY_WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass));
    load_str(h, KEY_IP_ADDR,   s_ip_addr, sizeof(s_ip_addr));
    load_str(h, KEY_IP_GW,     s_ip_gw,   sizeof(s_ip_gw));
    load_str(h, KEY_IP_MASK,   s_ip_mask, sizeof(s_ip_mask));
    load_str(h, KEY_IP_DNS,    s_ip_dns,  sizeof(s_ip_dns));

    uint8_t mode = s_ip_static ? 1 : 0;
    if (nvs_get_u8(h, KEY_IP_MODE, &mode) == ESP_OK) {
        s_ip_static = (mode != 0);
    }
    // A static mode with no address is unusable and would leave the device off
    // the network with no way back; fall back to DHCP instead.
    if (s_ip_static && s_ip_addr[0] == '\0') {
        ESP_LOGW(TAG, "static IP selected but no address stored, using DHCP");
        s_ip_static = false;
    }

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

const char *alt_settings_wifi_ssid(void) { return s_wifi_ssid; }
const char *alt_settings_wifi_pass(void) { return s_wifi_pass; }
bool alt_settings_ip_static(void)        { return s_ip_static; }
const char *alt_settings_ip_addr(void)   { return s_ip_addr; }
const char *alt_settings_ip_gw(void)     { return s_ip_gw; }
const char *alt_settings_ip_mask(void)   { return s_ip_mask; }
const char *alt_settings_ip_dns(void)    { return s_ip_dns; }

bool alt_settings_wifi_unconfigured(void) { return s_wifi_ssid[0] == '\0'; }

esp_err_t alt_settings_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
    // NULL keeps the stored password, so the SSID can be re-saved without
    // retyping it.
    if (pass) {
        strlcpy(s_wifi_pass, pass, sizeof(s_wifi_pass));
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_WIFI_SSID, s_wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_WIFI_PASS, s_wifi_pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved WiFi SSID \"%s\"", s_wifi_ssid);
    }
    return err;
}

esp_err_t alt_settings_set_ip(bool use_static, const char *addr, const char *gw,
                              const char *mask, const char *dns)
{
    if (use_static && (!addr || addr[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ip_static = use_static;
    if (addr) strlcpy(s_ip_addr, addr, sizeof(s_ip_addr));
    if (gw)   strlcpy(s_ip_gw,   gw,   sizeof(s_ip_gw));
    if (mask) strlcpy(s_ip_mask, mask, sizeof(s_ip_mask));
    if (dns)  strlcpy(s_ip_dns,  dns,  sizeof(s_ip_dns));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_IP_MODE, s_ip_static ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_IP_ADDR, s_ip_addr);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_IP_GW,   s_ip_gw);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_IP_MASK, s_ip_mask);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_IP_DNS,  s_ip_dns);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved IP mode %s%s%s", s_ip_static ? "static " : "DHCP",
                 s_ip_static ? "" : "", s_ip_static ? s_ip_addr : "");
    }
    return err;
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

// GPIO20, 24 and 28-31 are not bonded out on the ESP32, so they are not merely
// a bad choice - they do not exist.
static bool pin_exists(int p)
{
    if (p < 0 || p > 39)            return false;
    if (p == 20 || p == 24)         return false;
    if (p >= 28 && p <= 31)         return false;
    return true;
}

const char *alt_settings_check_pins(int rx, int tx)
{
    if (!pin_exists(rx) || !pin_exists(tx)) {
        return "no such GPIO on the ESP32";
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
