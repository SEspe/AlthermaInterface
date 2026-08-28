// AlthermaInterface - ESP-IDF firmware for the Daikin Altherma X10A service
// port. Protocol and value definitions derive from raomin/ESPAltherma (MIT);
// see README.md and docs/PORTING.md.
//
// Phase 3 (docs/PORTING.md): the X10A read path from phase 2, now publishing to
// MQTT. The poll loop is unchanged and still logs every decoded value over USB
// serial, so a decode problem stays distinguishable from a transport problem.

#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "althermaserial.h"
#include "board_config.h"
#include "converters.h"
#include "mqtt.h"
#include "settings.h"
#include "version.h"
#include "web_server.h"
#include "wifi.h"

static const char *TAG = "main";

// Upstream's FREQUENCY. Moves to NVS in phase 6.
#define ALT_POLL_INTERVAL_MS 30000

// EKHBH/EKHBX 008BA is R410A. The converter defaults to R32, which would skew
// every pressure->temperature conversion at 0x50, and no protocol-S definition
// file carries a convid 800-803 entry to correct it at runtime.
#define ALT_REFRIGERANT 801   // 801=R410A, 802=R32, 803=R22

#define ALT_MAX_REGISTRIES 32

static uint8_t s_registry_ids[ALT_MAX_REGISTRIES];
static size_t  s_registry_count;

static void log_labels_for(uint8_t reg_id)
{
    size_t total = converter_label_count();
    for (size_t i = 0; i < total; i++) {
        uint8_t reg;
        const char *label;
        const char *value;
        if (!converter_label_at(i, &reg, &label, &value) || reg != reg_id) {
            continue;
        }
        ESP_LOGI(TAG, "    %-32s %s", label, value);
    }
}

// Cross-check for the open question in docs/PORTING.md: PROTOCOL_S.h has no
// water temperatures, while the Rotex protocol-S variant maps inlet / outlet /
// DHW tank at 0x54 offsets 2, 4 and 8 (convid 153: unsigned 16-bit / 256).
// Print that alternative reading of the same bytes so the choice between the
// two definitions can be made from real values instead of the model number.
// Delete once the question is settled.
static void log_rotex_crosscheck(const uint8_t *buf, int len)
{
    // Protocol S payload starts at byte 1 (byte 0 is the registry ID).
    const int base = 1;
    if (len < base + 10) {
        return;
    }
    struct { int offset; const char *name; } probes[] = {
        {2, "Inlet water temp"},
        {4, "Outlet water temp"},
        {8, "DHW tank temp"},
    };
    ESP_LOGI(TAG, "  [Rotex cross-check on 0x54, convid 153]");
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const uint8_t *d = buf + base + probes[i].offset;
        double v = (double)((unsigned short)((d[1] << 8) | d[0])) / 256.0;
        ESP_LOGI(TAG, "    %-32s %.2f", probes[i].name, v);
    }
}

static void poll_once(char protocol)
{
    for (size_t i = 0; i < s_registry_count; i++) {
        uint8_t reg_id = s_registry_ids[i];
        uint8_t buf[ALT_REPLY_MAX];

        int len = -1;
        for (int tries = 0; tries < 3 && len < 0; tries++) {
            if (tries > 0) {
                ESP_LOGW(TAG, "reg 0x%02x: retrying (%d)", reg_id, tries);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            len = alt_query_registry(reg_id, buf, protocol);
        }
        if (len < 0) {
            ESP_LOGE(TAG, "reg 0x%02x: giving up this cycle", reg_id);
            continue;
        }

        // The reply must be for the registry we asked about. Protocol S puts
        // the ID in byte 0, protocol I in byte 1.
        uint8_t echoed = (protocol == 'S') ? buf[0] : buf[1];
        if (echoed != reg_id) {
            ESP_LOGE(TAG, "reg 0x%02x: reply claims 0x%02x, discarding", reg_id, echoed);
            continue;
        }

        converter_read_registry(buf, protocol);
        log_labels_for(reg_id);

        if (protocol == 'S' && reg_id == 0x54) {
            log_rotex_crosscheck(buf, len);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AlthermaInterface %s starting (reset reason %d)",
             FIRMWARE_VERSION, (int)esp_reset_reason());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %d core(s), rev %d.%d", chip.cores,
             chip.revision / 100, chip.revision % 100);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Rollback contract (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): an image that
    // reaches here has booted far enough to be worth keeping. Once OTA is in
    // use, move this call to after the first successful HP query so a firmware
    // that cannot talk to the heat pump rolls back instead.
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_ERROR_CHECK(alt_settings_init());
    alt_probe_rx_idle();   // sample RX before the UART driver claims the pin
    ESP_ERROR_CHECK(alt_serial_init());

    // WiFi and MQTT come up alongside the poll loop, not before it: the heat
    // pump is the point, and a broker that is down must not stop us reading and
    // logging values over serial.
    ESP_ERROR_CHECK(alt_wifi_start());
    if (!alt_wifi_wait_connected(30000)) {
        ESP_LOGW(TAG, "no WiFi after 30 s; carrying on regardless");
    }
    // Started unconditionally: esp-mqtt reconnects on its own once the network
    // appears, so a slow or absent AP must not permanently disable publishing.
    ESP_ERROR_CHECK(alt_mqtt_start());
    alt_mqtt_log_redirect();
    ESP_ERROR_CHECK(alt_web_start());

    converter_set_refrigerant(ALT_REFRIGERANT);
    char protocol = converter_protocol();

    s_registry_count = converter_registry_ids(s_registry_ids, ALT_MAX_REGISTRIES);
    if (s_registry_count == 0) {
        ESP_LOGE(TAG, "No values selected in the definition file. Nothing to poll.");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    char reglist[ALT_MAX_REGISTRIES * 5 + 1];
    alt_format_buffer(s_registry_ids, s_registry_count, reglist, sizeof(reglist));
    ESP_LOGI(TAG, "protocol '%c', refrigerant %d, %u labels over %u registries: %s",
             protocol, converter_refrigerant(), (unsigned)converter_label_count(),
             (unsigned)s_registry_count, reglist);

    while (true) {
        int64_t start = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "---- poll cycle ----");
        poll_once(protocol);

        alt_mqtt_publish_values();

        int64_t elapsed = (esp_timer_get_time() / 1000) - start;
        int64_t wait = ALT_POLL_INTERVAL_MS - elapsed;
        ESP_LOGI(TAG, "cycle took %lld ms, waiting %lld ms", elapsed, wait > 0 ? wait : 0);
        vTaskDelay(pdMS_TO_TICKS(wait > 0 ? wait : 0));
    }
}
