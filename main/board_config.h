#pragma once

// Pin map for the X10A service connector wiring, as actually wired on this
// board - NOT upstream ESPAltherma's src/setup.h defaults (which use 16/17).
// Keep pins here, not scattered through the code, so a different board is a
// one-file change (BirdBox convention).

// ---- X10A serial link (UART1, 9600 8E1) ----
#define ALT_UART_PORT      UART_NUM_1
#define ALT_UART_RX_PIN    16   // our RX  <- X10A TX
// GPIO15 is an ESP32 strapping pin (MTDO). Held LOW at reset it silences the
// ROM boot log on U0TXD; it has an internal pull-up and idle UART TX is high,
// so driving it as TX is fine - but if the boot log ever goes missing, suspect
// whatever is on the far end of this wire holding it down during reset.
#define ALT_UART_TX_PIN    15   // our TX  -> X10A RX
#define ALT_UART_BAUD      9600
// Upstream SER_TIMEOUT: how long the HP gets to answer a registry query.
#define ALT_UART_TIMEOUT_MS 300

// ---- Onboard OLED (SSD1306 128x64, I2C) ----
// The Lolin ESP32 boards used here carry a 0.96" panel wired to these pins.
// Confirmed empirically on the dev board, not taken from a datasheet: boards
// with onboard OLEDs disagree, and the Heltec/TTGO variant puts SDA on 4, SCL
// on 15 and the panel RESET on 16 - which would collide head-on with the X10A
// link above. This board is the Lolin map, so the two are independent.
//
// The panel is optional: the firmware probes ALT_OLED_ADDR at boot and runs
// unchanged if nothing answers, so one binary serves a board with a display and
// a plain WROOM devkit without one. Numeric port, not I2C_NUM_0, so this header
// stays includable without the I2C driver headers.
#define ALT_OLED_I2C_PORT  0
#define ALT_OLED_SDA_PIN   5
#define ALT_OLED_SCL_PIN   4
#define ALT_OLED_ADDR      0x3C

// ---- Thermostat dry contact ----
#define PIN_THERM              0
#define PIN_THERM_ACTIVE_STATE 1   // HIGH

// ---- Smart Grid dry contacts (optional; undefined = feature off) ----
// #define PIN_SG1  32
// #define PIN_SG2  33
#define SG_RELAY_HIGH_TRIGGER

#if defined(SG_RELAY_LOW_TRIGGER)
#define SG_RELAY_ACTIVE_STATE   0
#define SG_RELAY_INACTIVE_STATE 1
#else
#define SG_RELAY_ACTIVE_STATE   1
#define SG_RELAY_INACTIVE_STATE 0
#endif

// ---- Safety relay / preferred electricity tariff (optional) ----
// #define SAFETY_RELAY_PIN          33
// #define SAFETY_RELAY_ACTIVE_STATE 1

// ---- GitHub releases ----
// First-boot default for the repository the device may flash releases from;
// editable in the Config tab, stored in NVS thereafter.
#define ALT_GITHUB_REPO_DEFAULT "SEspe/AlthermaInterface"
