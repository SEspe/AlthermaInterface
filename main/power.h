#pragma once

// Power profiles, to reduce the load this board puts on the heat pump's
// internal 5 V regulator. That rail was never sized for an ESP32, and it is
// WiFi transmit bursts - a few hundred milliamps against a ~40 mA baseline -
// that stress a small regulator, so cutting transmit power buys more than
// shaving the idle draw does.
//
// Levels are cumulative and ordered least to most aggressive. Level 0 is the
// behaviour every release before 1.6.0 had, and remains the default: an OTA
// that silently cut transmit power could strand a unit with a marginal link
// inside a heat pump enclosure, recoverable only over serial or SoftAP.
//
// A level is applied at boot. The Config tab reboots on save, so there is no
// need to change any of this on a running system.
//
// NOT covered here: automatic light sleep, which is the largest saving
// available but needs the X10A UART moved off the APB clock first (see
// alt_power_cpu_mhz below) and a power-management lock held across each query.
// That is deliberately a separate change - it touches the field-proven serial
// path and has to be verified against the real heat pump.

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    ALT_POWER_OFF      = 0,  // 160 MHz, full transmit power
    ALT_POWER_BALANCED = 1,  // 160 MHz, transmit power reduced
    ALT_POWER_LOW      = 2,  // 80 MHz, transmit power reduced
    ALT_POWER_LEVEL_COUNT
} alt_power_level_t;

bool alt_power_level_valid(int level);

// Short name and one-line description, shared by the web UI and the boot log
// so the two can never drift apart.
const char *alt_power_level_name(int level);
const char *alt_power_level_detail(int level);

// CPU frequency in MHz for a level.
//
// Only 80, 160 and 240 MHz are used, and all three are derived from the PLL, so
// the APB clock stays at 80 MHz whichever is chosen. That is what makes these
// levels safe for the X10A UART: its baud divisor is derived from APB, and APB
// only follows the CPU when the chip drops to an XTAL frequency such as 40 MHz.
// Any future level that goes below 80 MHz MUST move the UART to
// UART_SCLK_REF_TICK first, or 9600 8E1 will drift and the link will start
// failing its CRC.
int alt_power_cpu_mhz(int level);

// Maximum WiFi transmit power in units of 0.25 dBm, as esp_wifi takes it.
int alt_power_tx_quarter_dbm(int level);

// Applies the CPU part of a level. Safe before WiFi is up.
esp_err_t alt_power_apply_cpu(int level);

// Applies the WiFi part. Must come after esp_wifi_start(), which is when the
// transmit power can first be set.
esp_err_t alt_power_apply_wifi(int level);
