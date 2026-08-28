#pragma once

// Pin map for the X10A service connector wiring. Values match upstream
// ESPAltherma's src/setup.h defaults for an ESP32 devkit (RX_PIN/TX_PIN);
// keep them here, not scattered through the code, so a different board is a
// one-file change (BirdBox convention).

// ---- X10A serial link (UART1, 9600 8E1) ----
#define ALT_UART_PORT      UART_NUM_1
#define ALT_UART_RX_PIN    16   // to the TX pin of X10A
#define ALT_UART_TX_PIN    17   // to the RX pin of X10A
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
