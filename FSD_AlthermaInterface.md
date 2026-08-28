# FSD — AlthermaInterface

**Version:** 0.1
**Firmware:** 0.1.0
**Target:** ESP32 (ESP32-WROOM devkit, 4 MB flash), ESP-IDF v6.0.1

Functional Specification + change record for AlthermaInterface. This document
is authoritative for *what the firmware must do*; `docs/PORTING.md` covers *how
the upstream code maps onto it*, and `CLAUDE.md` covers build/verify workflow.

## Changelog

- v0.1 — **ESP-IDF scaffold (firmware 0.1.0), §1–§9.** New repo established as
  a native ESP-IDF rewrite of raomin/ESPAltherma (MIT), which is credited in
  `README.md` and kept as a git-ignored reference clone in
  `upstream-ESPAltherma/`. Project skeleton builds for target `esp32`: root
  `CMakeLists.txt`, `main/` component with `main.c` + `board_config.h` +
  `version.h`, `sdkconfig.defaults(.esp32)`, dual-OTA `partitions-esp32.csv`,
  `espressif/mqtt` pulled from the component registry (esp-mqtt is no longer
  bundled in IDF 6.x). No functional module ported yet.

---

## 1. Purpose

Read every available operating value from a Daikin Altherma heat pump via its
X10A service connector and expose them to Home Assistant over MQTT, plus
control a small number of dry-contact outputs (thermostat, Smart Grid, safety
relay). Same job as upstream ESPAltherma; different foundation.

## 2. Scope

**In scope:** ESP32 targets, X10A protocols I and S, MQTT publish + Home
Assistant discovery, dry-contact outputs, runtime configuration, OTA.

**Out of scope:** ESP8266, M5Stack/M5StickC screens and battery telemetry,
Daikin models outside the upstream definition set.

## 3. Hardware interface

### 3.1 X10A serial link
9600 baud, 8E1, on UART1 — `RX = GPIO16` (to X10A TX), `TX = GPIO17` (to X10A
RX). Pin map in `main/board_config.h`. X10A supplies 5 V; the ESP is powered
from it in the usual installation.

### 3.2 Dry-contact outputs
- **Thermostat** (`PIN_THERM`, default GPIO0, active HIGH) — normally-open
  contact, state persisted across reboots.
- **Smart Grid** (`PIN_SG1`/`PIN_SG2`, optional) — four states: Free Running,
  Forced Off, Recommended On, Forced On.
- **Safety relay / preferred tariff** (`SAFETY_RELAY_PIN`, optional) — stops
  the heat pump when triggered.

Every output must be driven to its inactive state before its pin is configured
as an output, so that boot-time pin float cannot command the heat pump.

## 4. Query cycle

Poll each distinct registry named by the selected model definition, in turn,
every `FREQUENCY` ms (default 30 000). Per registry: send the framed query,
read the reply within 300 ms, verify CRC, retry up to 3× on failure, decode the
values, accumulate them into one JSON object; publish the object once per cycle.

## 5. Protocols

- **Protocol I** (default): query `0x03 0x40 <regID> <CRC>`; reply length is
  read from byte 3 of the response.
- **Protocol S**: query `0x02 <regID> <CRC>`; reply length is fixed per
  registry (0x50 → 6, 0x56 → 4, otherwise 18).
- CRC on both: `~(sum of bytes)`.
- Error reply `0x15 0xEA` = the heat pump did not understand the command.

Reference: upstream `doc/Daikin I protocol.md`, `doc/Daikin S protocol.md`.

## 6. Value decoding

Registry bytes → labelled values via the upstream converter table (~100
conversion IDs) and the per-model definition files. Refrigerant type
(R410A/R32/R22) selects the pressure→temperature curve.

## 7. MQTT

Topics follow upstream so an existing Home Assistant setup keeps working:
`espaltherma/ATTR` (JSON of all values), `espaltherma/LWT` (Online/Offline,
retained), `espaltherma/POWER` + `/STATE` (thermostat), `espaltherma/sg/set` +
`/state`, `espaltherma/log`. Home Assistant discovery is published retained
under `homeassistant/…` on each connect.

Deviation from upstream: TLS connections verify the broker via the IDF
certificate bundle rather than `setInsecure()`.

## 8. Configuration

Upstream requires editing `src/setup.h` and reflashing. Here, WiFi credentials,
MQTT broker/credentials, poll frequency and output pin roles live in NVS and
are editable from an on-device web UI. Model definition selection stays
compile-time until further notice (see `docs/PORTING.md`, open questions).

## 9. Update & recovery

Dual OTA app slots with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: an image that
does not confirm itself valid is rolled back on the next boot. Target: mark the
image valid only after the first successful X10A query, so firmware that cannot
talk to the heat pump rolls itself back.
