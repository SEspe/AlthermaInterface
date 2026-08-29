#include "power.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_wifi.h"

static const char *TAG = "power";

// 20 dBm is the ESP32 default and the maximum esp_wifi accepts; 13 dBm is a
// 7 dB cut, which is roughly a quarter of the radiated power and close to half
// the transmit current. At the reference unit's -61 dBm RSSI that leaves a wide
// margin, but it is the setting most likely to matter on a weak link, which is
// why it does not apply at level 0.
#define TX_FULL_QUARTER_DBM  80  // 20.0 dBm
#define TX_CUT_QUARTER_DBM   52  // 13.0 dBm

struct level_def {
    const char *name;
    const char *detail;
    int cpu_mhz;
    int tx_quarter_dbm;
};

static const struct level_def s_levels[ALT_POWER_LEVEL_COUNT] = {
    [ALT_POWER_OFF] = {
        "Off",
        "160 MHz, full transmit power. The behaviour of every release before "
        "1.6.0, and the safest choice on a weak WiFi link.",
        160, TX_FULL_QUARTER_DBM,
    },
    [ALT_POWER_BALANCED] = {
        "Balanced",
        "160 MHz, transmit power 13 dBm instead of 20. Cuts the transmit "
        "current bursts, which is what a small regulator actually feels.",
        160, TX_CUT_QUARTER_DBM,
    },
    [ALT_POWER_LOW] = {
        "Low",
        "80 MHz and transmit power 13 dBm. The heat pump link is 9600 baud and "
        "is polled every 30 s, so half the clock is still far more than this "
        "firmware needs.",
        80, TX_CUT_QUARTER_DBM,
    },
};

bool alt_power_level_valid(int level)
{
    return level >= 0 && level < ALT_POWER_LEVEL_COUNT;
}

const char *alt_power_level_name(int level)
{
    return alt_power_level_valid(level) ? s_levels[level].name : "unknown";
}

const char *alt_power_level_detail(int level)
{
    return alt_power_level_valid(level) ? s_levels[level].detail : "";
}

int alt_power_cpu_mhz(int level)
{
    return alt_power_level_valid(level) ? s_levels[level].cpu_mhz
                                        : s_levels[ALT_POWER_OFF].cpu_mhz;
}

int alt_power_tx_quarter_dbm(int level)
{
    return alt_power_level_valid(level) ? s_levels[level].tx_quarter_dbm
                                        : s_levels[ALT_POWER_OFF].tx_quarter_dbm;
}

esp_err_t alt_power_apply_cpu(int level)
{
    if (!alt_power_level_valid(level)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int mhz = alt_power_cpu_mhz(level);

    // min == max, so no dynamic frequency scaling happens and the APB clock
    // never moves. Scaling is what would endanger the UART baud rate; a fixed
    // lower frequency does not. light_sleep_enable stays false for the same
    // reason - a sleeping chip would miss the heat pump's reply.
    esp_pm_config_t pm = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        // Not fatal: the firmware runs perfectly well at the compiled-in
        // frequency, and refusing to boot over a power preference would be a
        // poor trade for a device inside a heat pump.
        ESP_LOGW(TAG, "cpu %d MHz rejected: %s", mhz, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "level %d (%s): cpu %d MHz", level, alt_power_level_name(level), mhz);
    return ESP_OK;
}

esp_err_t alt_power_apply_wifi(int level)
{
    if (!alt_power_level_valid(level)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int q = alt_power_tx_quarter_dbm(level);

    // Modem sleep between DTIM beacons. This is already the esp_wifi default,
    // but it is set explicitly so the level is what decides, not the default of
    // whichever IDF version this was built against.
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ps failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_max_tx_power((int8_t)q);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tx power %d (%.2f dBm) rejected: %s",
                 q, q / 4.0, esp_err_to_name(err));
        return err;
    }

    // Read back rather than trusting the write: esp_wifi rounds to the levels
    // the radio actually supports, so the requested value and the effective one
    // are not always the same number.
    int8_t got = 0;
    if (esp_wifi_get_max_tx_power(&got) == ESP_OK) {
        ESP_LOGI(TAG, "level %d (%s): tx power %.2f dBm, modem sleep on",
                 level, alt_power_level_name(level), got / 4.0);
    }
    return ESP_OK;
}
