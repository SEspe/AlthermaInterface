# FSD — AlthermaInterface

**Version:** 0.5
**Firmware:** 0.3.0
**Target:** ESP32 (ESP32-WROOM devkit, 4 MB flash), ESP-IDF v6.0.1
**Heat pump:** Daikin Altherma LT split hydrobox **EKHBH / EKHBX 008BA** —
**protocol S**

Functional Specification + change record for AlthermaInterface. This document
is authoritative for *what the firmware must do*; `docs/PORTING.md` covers *how
the upstream code maps onto it*, and `CLAUDE.md` covers build/verify workflow.

## Changelog

- v0.5 — **WiFi, MQTT and the web UI (firmware 0.3.0), §7, §8, §10.** Phase 3,
  plus the web UI that was scheduled for phase 6 and pulled forward.
  `wifi.c` (station mode, event-group state, upstream's reconnect ladder:
  re-associate every 15 s, reboot after 2 min with no link), `mqtt.c`
  (esp-mqtt; one JSON object per cycle to `espaltherma/ATTR` in upstream's exact
  format, retained `espaltherma/LWT`, log mirror on `espaltherma/log`),
  `settings.c` (MQTT broker/user/password in NVS) and `web_server.c` (four tabs:
  Daikin Data, WiFi, Config, OTA, with `POST /ota/upload`).
  Configuration is now genuinely runtime: `secrets.h` supplies only first-boot
  defaults, and anything saved from the Config tab overrides it from NVS.
  The stored MQTT password is never sent to the browser — the UI is told only
  whether one exists, and a blank field means "keep it".
  Upstream's same-SSID AP roaming is **not** ported; noted in `wifi.c`.
  Home Assistant discovery is still phase 4, so entities must be defined by
  hand for now.

- v0.4 — **X10A read path (firmware 0.2.0), §4, §5, §6.** Phase 2 of the port.
  `main/althermaserial.c` (UART1 9600 8E1, protocol I/S framing, CRC, timeout
  and retry) and `main/converters.cpp` (the full upstream conversion table
  behind a C facade) replace the skeleton loop; `main.c` now polls every
  registry named by the definition file and logs each decoded value over USB
  serial. No WiFi, no MQTT yet — deliberately, so a bad reading can only be the
  wiring, the protocol or the definition.
  Refrigerant is pinned to R410A (801) in `main.c`: the converter defaults to
  R32 and no protocol-S definition carries a convid 800-803 entry to correct it.
  Three upstream quirks are documented in `docs/PORTING.md`; one — an
  out-of-bounds table read on `0x55` "Operation Mode" — is fixed rather than
  preserved.
  Also fixes `CONFIG_MQTT_BUFFER_SIZE`, which was being silently discarded
  because it depends on `CONFIG_MQTT_USE_CUSTOM_CONFIG`; takes effect when
  `sdkconfig` is next regenerated, before phase 3 needs it.
  Verified: builds clean, boots, opens UART1 on GPIO16/15, enumerates 25 labels
  over registries 0x50/0x53/0x54/0x55, and times out cleanly with no heat pump
  attached. **GPIO15-to-GPIO16 loopback passed** — each query echoed back
  verbatim (`0x02 0x50 0xad`, `0x02 0x53 0xaa`, `0x02 0x54 0xa9`,
  `0x02 0x55 0xa8`), all four CRCs correct by hand, which proves both pins, the
  9600 8E1 setup and the protocol-S framing independently of the heat pump.
  **Not yet verified against the heat pump.**

- v0.3 — **X10A TX moved to GPIO15 (firmware 0.1.0), §3.1.** As-wired pin map
  is RX = GPIO16, **TX = GPIO15**, not upstream's 16/17. GPIO15 is a strapping
  pin (MTDO) — safe as a UART TX output, but recorded here because a low on it
  at reset silences the ROM boot log and that failure looks like a dead board.
  Firmware version unchanged: nothing in the build opens the UART yet.

- v0.2 — **Target unit fixed: EKHBH/EKHBX 008BA, protocol S (firmware 0.1.0),
  §5, §6.** The unit is a BA-generation Altherma LT hydrobox, which speaks the
  *older* protocol S, not protocol I — upstream documents this exact family
  (`doc/Daikin S protocol.md`: "DAIKIN EKHBH016BA6WN year 2009", issue #46).
  `main/def/PROTOCOL_S.h` + `main/labeldef.h` copied from upstream, selected via
  the new `main/model_config.h`. Consequence, recorded here because it sets
  expectations: protocol S exposes **25 values across 4 registries**, not the
  hundreds a protocol-I machine offers. Not yet in the build — no consumer of
  the table exists until the converter lands.

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
9600 baud, 8E1, on UART1 — **`RX = GPIO16`** (← X10A TX), **`TX = GPIO15`**
(→ X10A RX). This is the wiring on the actual board and differs from upstream's
16/17 default. Pin map in `main/board_config.h`. X10A supplies 5 V; the ESP is
powered from it in the usual installation.

GPIO15 is a strapping pin (MTDO): held low at reset it silences the ROM boot
log on U0TXD. Idle UART TX is high and the pin has an internal pull-up, so this
is safe — but a missing boot log points here.

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

**This unit uses protocol S** (EKHBH/EKHBX 008BA, BA generation). Protocol I is
implemented anyway — it is a handful of lines difference and keeps the firmware
usable on a newer machine.

- **Protocol S** (this unit): query `0x02 <regID> <CRC>`, 3 bytes. Reply length
  is fixed per registry, and the registry ID is the **first** byte of the reply.
- **Protocol I**: query `0x03 0x40 <regID> <CRC>`, 4 bytes. Reply length is read
  from byte 3 of the response; the registry ID is the **second** byte.
- CRC on both: `~(sum of bytes)`.
- Error reply `0x15 0xEA` = the heat pump did not understand the command.

Reply lengths for protocol S: upstream's `get_reply_len()` returns 6 for `0x50`,
**4** for `0x56`, 18 otherwise, while `doc/Daikin S protocol.md` states 6 for
both `0x50` and `0x56`. The code is field-proven and wins; `0x56` is moot here
because `PROTOCOL_S.h` never queries it.

Reference: upstream `doc/Daikin S protocol.md`, `doc/Daikin I protocol.md`.

## 6. Value decoding

Registry bytes → labelled values via the upstream converter table (~100
conversion IDs) and the per-model definition file, selected in
`main/model_config.h`.

For this unit that file is `main/def/PROTOCOL_S.h`: **25 values over 4
registries** — `0x50` (HP/LP refrigerant pressure), `0x53` (expansion valve, fan
speeds, compressor frequency, output states), `0x54` (the temperatures: outdoor
air, indoor suction, both heat exchangers, discharge pipe, fin, setpoint),
`0x55` (operation mode, error / thermo-off / warning / caution codes).

This is far less than a modern protocol-I Altherma reports (hundreds of values),
and it is a property of the 2009-era PCB, not of this firmware. Notably absent:
leaving/entering water temperature and DHW tank temperature — the Rotex
protocol-S variant maps those at `0x54` offsets 2/4/8, so if this unit turns out
to answer with plausible water temperatures there, `PROTOCOL_S_ROTEX.h` is the
better base. Decide empirically in phase 2, not from the model number.

Refrigerant type (R410A/R32/R22) selects the pressure→temperature curve;
EKHBH/EKHBX 008BA is **R410A**.

## 7. MQTT

Topics follow upstream so an existing Home Assistant setup keeps working:
`espaltherma/ATTR` (JSON of all values), `espaltherma/LWT` (Online/Offline,
retained), `espaltherma/POWER` + `/STATE` (thermostat), `espaltherma/sg/set` +
`/state`, `espaltherma/log`. Home Assistant discovery is published retained
under `homeassistant/…` on each connect.

Deviation from upstream: TLS connections verify the broker via the IDF
certificate bundle rather than `setInsecure()`.

## 8. Configuration

Upstream requires editing `src/setup.h` and reflashing. Here the MQTT broker
URI, username and password live in NVS and are edited from the Config tab
(`main/settings.c`). `main/secrets.h` — git-ignored, with a committed
`secrets.h.example` — supplies first-boot defaults only; once saved from the web
UI, NVS wins.

The stored MQTT password is never sent to the browser. `GET /api/config`
reports only whether one exists, and a blank password field on save means "keep
the stored one", so the broker address can be changed without retyping it.

Still compile-time, to be moved later: WiFi credentials (in `secrets.h`), poll
frequency, output pin roles, and the model definition selection (see
`docs/PORTING.md`, open questions).

## 10. Web UI

Served by the device itself on port 80 (`main/web_server.c`), four tabs:

- **Daikin Data** — every label that has been read at least once, with its
  registry and current value; refreshes every 5 s.
- **WiFi** — SSID, IP, RSSI, channel, BSSID, link and broker state, free heap,
  uptime, firmware version.
- **Config** — MQTT broker URI, username, password. Saving persists to NVS and
  reboots, because settings are read once at start-up and a restart is the one
  path guaranteed to be consistent.
- **OTA** — firmware upload with a progress bar.

Endpoints: `GET /`, `GET /api/status`, `GET /api/values`, `GET|POST
/api/config`, `POST /ota/upload`.

There is **no authentication**. The device is expected to sit on a trusted LAN,
exactly as upstream's ArduinoOTA does. Anyone who can reach port 80 can reflash
it — do not expose it to the internet.

## 9. Update & recovery

Dual OTA app slots with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: an image that
does not confirm itself valid is rolled back on the next boot. Target: mark the
image valid only after the first successful X10A query, so firmware that cannot
talk to the heat pump rolls itself back.
