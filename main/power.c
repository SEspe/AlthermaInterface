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
    wifi_ps_type_t ps;
};

static const struct level_def s_levels[ALT_POWER_LEVEL_COUNT] = {
    [ALT_POWER_OFF] = {
        "Off",
        "160 MHz, full transmit power, no modem sleep. The safest choice on a "
        "weak link, and the most responsive web UI.",
        160, TX_FULL_QUARTER_DBM, WIFI_PS_NONE,
    },
    [ALT_POWER_BALANCED] = {
        "Balanced",
        "160 MHz, transmit power 13 dBm instead of 20, modem sleep on. Cuts the "
        "transmit current bursts, which is what a small regulator feels.",
        160, TX_CUT_QUARTER_DBM, WIFI_PS_MIN_MODEM,
    },
    [ALT_POWER_LOW] = {
        "Low",
        "80 MHz, transmit power 13 dBm, modem sleep on. The heat pump link is "
        "9600 baud and polled once a minute, so half the clock is still far "
        "more than this firmware needs.",
        80, TX_CUT_QUARTER_DBM, WIFI_PS_MIN_MODEM,
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

int alt_power_wifi_ps(int level)
{
    return alt_power_level_valid(level) ? (int)s_levels[level].ps
                                        : (int)s_levels[ALT_POWER_OFF].ps;
}

// Applied only once the station has an address. Modem sleep parks the radio
// between DTIM beacons, and a DHCP OFFER or ACK arriving in that window can be
// missed - which depends entirely on how the access point buffers frames, so it
// works on one network and fails on the next. The sibling BirdBox project
// disables power save outright for the same reason ("modem-sleep latency ruins
// HTTP"). Associating and leasing at full power costs a few hundred
// milliseconds once per boot and removes the failure completely.
esp_err_t alt_power_ps_off(void)
{
    wifi_ps_type_t cur = WIFI_PS_NONE;
    // Only log a transition; this is called on every disconnect and would
    // otherwise be noise on a flapping link.
    if (esp_wifi_get_ps(&cur) == ESP_OK && cur == WIFI_PS_NONE) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "modem sleep off (no IP held)");
    }
    return err;
}

esp_err_t alt_power_apply_ps(int level)
{
    wifi_ps_type_t ps = (wifi_ps_type_t)alt_power_wifi_ps(level);
    esp_err_t err = esp_wifi_set_ps(ps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ps failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "level %d (%s): modem sleep %s",
             level, alt_power_level_name(level),
             ps == WIFI_PS_NONE ? "off" : "on");
    return ESP_OK;
}

esp_err_t alt_power_apply_wifi(int level)
{
    if (!alt_power_level_valid(level)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int q = alt_power_tx_quarter_dbm(level);

    // Power save is deliberately OFF here regardless of level: the station has
    // not associated or leased an address yet, and modem sleep during the DHCP
    // handshake is what made this unit look like it was being refused a lease.
    // The level's real setting is applied from the got-IP handler instead.
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ps(NONE) failed: %s", esp_err_to_name(err));
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
        // Power save deliberately not mentioned here: at this point it is always
        // off, whatever the level. The level's setting is logged by
        // alt_power_apply_ps() once a lease actually exists.
        ESP_LOGI(TAG, "level %d (%s): tx power %.2f dBm requested %.2f",
                 level, alt_power_level_name(level), got / 4.0, q / 4.0);
    }
    return ESP_OK;
}
