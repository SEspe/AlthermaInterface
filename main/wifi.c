// Ported from the WiFi handling in upstream ESPAltherma src/main.cpp
// (MIT, (c) 2020 Raomin).
//
// Upstream drives WiFi from a blocking loop: checkWifi() spins on
// WiFi.status(), forcing a full rescan after 15 s and rebooting after 2 min.
// The recovery *policy* is worth keeping - a heat pump interface that quietly
// stops reporting is worse than one that reboots - but the spin is not. Here
// the same ladder runs off esp_wifi's event handlers, so nothing blocks.
//
// Upstream's same-SSID AP roaming (rescan when RSSI < -75 dBm, switch if
// another AP is >= 8 dB better) is NOT ported yet. It only matters with several
// APs sharing one SSID, and it complicates the state machine; revisit if the
// unit turns out to sit at the edge of two APs.

#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "settings.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0

// Upstream's thresholds: force a fresh association attempt every 15 s, and give
// up on the whole stack after 2 minutes of no link.
#define WIFI_RETRY_INTERVAL_MS   15000
#define WIFI_REBOOT_AFTER_MS    120000
// One DHCP restart before giving up on the lease; some APs associate a client
// and then never answer its first DISCOVER.
#define WIFI_DHCP_KICK_AFTER_MS  25000

static EventGroupHandle_t s_wifi_events;
static int64_t s_down_since_ms;      // 0 once we have an IP
static esp_timer_handle_t s_retry_timer;

// Link-layer association, tracked separately from having an IP address.
// Conflating the two is a trap: the station reaches "run" within a few seconds
// while DHCP can still take a while, and re-associating during that window
// tears down a perfectly good link and restarts the wait - forever.
static volatile bool s_associated;
static bool s_dhcp_kicked;
static bool s_ap_mode;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void retry_timer_cb(void *arg)
{
    (void)arg;

    if (alt_wifi_is_connected()) {
        return;
    }

    int64_t down_for = now_ms() - s_down_since_ms;

    if (down_for >= WIFI_REBOOT_AFTER_MS) {
        // The stack is wedged rather than the AP being away: upstream reboots
        // here and so do we. A heat pump interface that has silently stopped
        // reporting is worse than one that restarts.
        ESP_LOGE(TAG, "no usable link for %lld ms, rebooting", down_for);
        esp_restart();
    }

    if (s_associated) {
        // Associated but no IP yet. Calling esp_wifi_connect() here would drop
        // a working association and start over, so instead report what the DHCP
        // client thinks it is doing and, once, kick it.
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
        if (netif) {
            esp_netif_dhcpc_get_status(netif, &st);
        }
        ESP_LOGW(TAG, "associated but no IP after %lld ms (dhcpc status %d)", down_for, (int)st);

        if (netif && !s_dhcp_kicked && down_for >= WIFI_DHCP_KICK_AFTER_MS) {
            s_dhcp_kicked = true;
            ESP_LOGW(TAG, "restarting DHCP client");
            esp_netif_dhcpc_stop(netif);
            esp_netif_dhcpc_start(netif);
        }
        return;
    }

    ESP_LOGW(TAG, "not associated after %lld ms, retrying", down_for);
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        s_down_since_ms = now_ms();
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        s_associated = true;
        ESP_LOGI(TAG, "associated, waiting for DHCP");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        s_associated = false;
        if (s_down_since_ms == 0) {
            s_down_since_ms = now_ms();
        }
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting", e ? e->reason : -1);
        esp_wifi_connect();
        break;
    }

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    s_down_since_ms = 0;
    s_dhcp_kicked = false;
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "connected to \"%s\", IP " IPSTR ", RSSI %d dBm",
             alt_settings_wifi_ssid(), IP2STR(&e->ip_info.ip), alt_wifi_rssi());
}

esp_err_t alt_wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    // No SSID configured: bring up the provisioning SoftAP instead of trying to
    // associate with nothing. APSTA rather than plain AP, so the Config tab can
    // still scan for networks while serving the page over the AP.
    if (alt_settings_wifi_unconfigured()) {
        s_ap_mode = true;
        esp_netif_t *ap = esp_netif_create_default_wifi_ap();

        esp_netif_ip_info_t ip = {0};
        ip.ip.addr      = esp_ip4addr_aton(ALT_AP_IP);
        ip.gw.addr      = esp_ip4addr_aton(ALT_AP_IP);
        ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        esp_netif_dhcps_stop(ap);
        esp_netif_set_ip_info(ap, &ip);
        esp_netif_dhcps_start(ap);

        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));

        wifi_config_t apcfg = {0};
        strlcpy((char *)apcfg.ap.ssid, ALT_AP_SSID, sizeof(apcfg.ap.ssid));
        apcfg.ap.ssid_len = strlen(ALT_AP_SSID);
        apcfg.ap.channel = 1;
        apcfg.ap.max_connection = 4;
        // Open on purpose: this exists so someone with no credentials can get
        // in and supply them. It is only up until a network is configured.
        apcfg.ap.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apcfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGW(TAG, "no WiFi configured - provisioning AP \"%s\" at %s",
                 ALT_AP_SSID, ALT_AP_IP);
        return ESP_OK;
    }

    if (alt_settings_ip_static()) {
        // Worth having: a router that associates a client but never answers its
        // DHCP DISCOVER leaves the device unreachable with nothing wrong on
        // this side - which is exactly what this unit's AP does.
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr      = esp_ip4addr_aton(alt_settings_ip_addr());
        ip.gw.addr      = esp_ip4addr_aton(alt_settings_ip_gw());
        ip.netmask.addr = esp_ip4addr_aton(alt_settings_ip_mask());

        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta, &ip));

        if (alt_settings_ip_dns()[0]) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(alt_settings_ip_dns());
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
        }

        ESP_LOGI(TAG, "static IP %s, gw %s, mask %s, dns %s",
                 alt_settings_ip_addr(), alt_settings_ip_gw(),
                 alt_settings_ip_mask(), alt_settings_ip_dns());
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, alt_settings_wifi_ssid(), sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, alt_settings_wifi_pass(), sizeof(cfg.sta.password));
    // Upstream sets WIFI_ALL_CHANNEL_SCAN + sort by signal so it associates to
    // the strongest AP of the SSID rather than the first one heard.
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    const esp_timer_create_args_t targs = {
        .callback = &retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_retry_timer,
                                             (uint64_t)WIFI_RETRY_INTERVAL_MS * 1000));

    ESP_LOGI(TAG, "station started, connecting to \"%s\"", alt_settings_wifi_ssid());
    return ESP_OK;
}

bool alt_wifi_ap_mode(void) { return s_ap_mode; }

int alt_wifi_scan_json(char *out, size_t len)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        snprintf(out, len, "[]");
        return 0;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) {
        n = 32;
    }
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        snprintf(out, len, "[]");
        return 0;
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    // esp_wifi already returns these strongest-first. Several APs can share one
    // SSID, so keep only the best of each - the list is for choosing a network,
    // not an access point.
    size_t pos = 0;
    int written = 0;
    pos += snprintf(out + pos, len - pos, "[");
    for (uint16_t i = 0; i < n && pos + 96 < len; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strcmp(ssid, (const char *)recs[j].ssid) == 0) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        pos += snprintf(out + pos, len - pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"open\":%s}",
                        written ? "," : "", ssid, recs[i].rssi, recs[i].primary,
                        recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        written++;
    }
    snprintf(out + pos, len - pos, "]");

    free(recs);
    esp_wifi_scan_stop();
    ESP_LOGI(TAG, "scan found %d networks", written);
    return written;
}

bool alt_wifi_is_connected(void)
{
    if (!s_wifi_events) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool alt_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_events) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

int alt_wifi_rssi(void)
{
    if (!alt_wifi_is_connected()) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

void alt_wifi_ip(char *out, size_t len)
{
    strlcpy(out, "0.0.0.0", len);
    if (!alt_wifi_is_connected()) {
        return;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(out, len, IPSTR, IP2STR(&info.ip));
    }
}

const char *alt_wifi_ssid(void)
{
    return s_ap_mode ? ALT_AP_SSID : alt_settings_wifi_ssid();
}

int alt_wifi_channel(void)
{
    wifi_ap_record_t ap;
    if (!alt_wifi_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.primary;
}

void alt_wifi_bssid(char *out, size_t len)
{
    strlcpy(out, "--", len);
    wifi_ap_record_t ap;
    if (!alt_wifi_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             ap.bssid[0], ap.bssid[1], ap.bssid[2],
             ap.bssid[3], ap.bssid[4], ap.bssid[5]);
}
