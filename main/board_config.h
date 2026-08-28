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
