// AlthermaInterface - ESP-IDF firmware for the Daikin Altherma X10A service
// port. Protocol and value definitions derive from raomin/ESPAltherma (MIT);
// see README.md and docs/PORTING.md.

#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "version.h"

static const char *TAG = "main";

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
    // reaches here has booted far enough to be worth keeping. Once the MQTT and
    // X10A paths exist, move this call to after the first successful HP query so
    // a firmware that can't talk to the heat pump rolls back instead.
    esp_ota_mark_app_valid_cancel_rollback();

    // Port order (docs/PORTING.md):
    //   1. althermaserial - X10A UART + protocol I/S framing and CRC
    //   2. converters     - registry bytes -> labelled values
    //   3. wifi / mqtt    - esp_wifi station, esp-mqtt publisher, HA discovery
    //   4. control        - thermostat / smart grid / safety relay outputs
    //   5. web_server     - runtime config UI + OTA upload
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "idle - firmware skeleton, no modules ported yet");
    }
}
