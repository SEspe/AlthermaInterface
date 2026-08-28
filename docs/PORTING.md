# Porting ESPAltherma (Arduino/PlatformIO) to AlthermaInterface (ESP-IDF)

Upstream: `raomin/ESPAltherma` @ `4e518ec` (2026-07-24), MIT. Reference clone
lives at `upstream-ESPAltherma/` and is git-ignored — it is **input**, not a
subtree of this repo.

Upstream is small: ~2 100 lines across 10 files, plus ~40 generated definition
headers. Almost all of the value is in the protocol + conversion tables, which
are framework-independent; the framework coupling is concentrated in WiFi,
MQTT, EEPROM, OTA and `Serial`.

## File-by-file map

| Upstream | New home | Effort | Notes |
|---|---|---|---|
| `include/comm.h` (135) | `main/althermaserial.c/.h` | medium | `HardwareSerial` → `driver/uart.h`. CRC and the protocol I/S framing port verbatim. `millis()` deadlines → `esp_timer_get_time()`. Drop the ESP8266 `SoftwareSerial` branch. |
| `include/converters.h` (610) | `main/converters.cpp/.h` | low | Pure computation over a byte buffer. Keep as C++ (like BirdBox's `classify.cpp`) so the `Converter` class and `labelDefs[]` array need no rewrite. Only `Serial.printf` debug lines change to `ESP_LOGx`. |
| `include/labeldef.h` | `main/labeldef.h` | trivial | Drop `<pgmspace.h>`; the class itself is plain C++. |
| `include/def/*.h` (40 files) | `main/def/*.h` | none | Copy as-is. Model selection stays compile-time for now (§ open question below). |
| `include/mqtt.h` (364) | `main/mqtt.c` + `main/control.c` | **high** | PubSubClient → `esp_mqtt_client`. Callback-driven subscribe handling → `MQTT_EVENT_DATA` in an event handler. `beginPublish/write/endPublish` chunked HA discovery → `esp_mqtt_client_publish` with a pre-sized buffer, or keep chunking via `esp_mqtt_client_enqueue`. Relay/thermostat command handling splits out into `control.c`. |
| `include/mqttserial.h` (79) | `main/log_mqtt.c` | medium | The `Stream` subclass that mirrors logs to MQTT becomes an `esp_log_set_vprintf()` hook. Keep the "only publish when connected" guard — it prevents a reconnect storm. |
| `src/main.cpp` (491) | `main/main.c`, `main/wifi.c` | **high** | `setup()`/`loop()` → `app_main()` + a poll task. `WiFi.begin`/`status`/roaming → `esp_wifi` + event group; the strongest-AP roaming logic (`checkWifiRoaming`) maps onto `esp_wifi_scan_start` + `esp_wifi_connect` with an explicit BSSID. `delay()` spin loops → `vTaskDelay`. |
| `src/setup.h` (137) | `main/settings.c/.h` + `main/board_config.h` | **high** | Pins and feature flags → `board_config.h` (done). Credentials, MQTT host, poll frequency → NVS, editable at runtime; this is the main functional gain over upstream. |
| `src/homeassistant.cpp` (190) + `include/homeassistant.h` | `main/homeassistant.cpp/.h` | low | Already framework-free (`std::string`, a sink callback). Ports near-verbatim. |
| `include/restart.h` | — | none | `esp_restart()` directly. |
| `scripts/`, `contrib/`, `test/` | not ported | — | Off-device Python tooling; run from the upstream clone if needed. |
| — | `main/web_server.c` | new | Config UI + OTA upload, BirdBox pattern. Not in upstream. |

## API replacement table

| Arduino | ESP-IDF |
|---|---|
| `HardwareSerial`, `Serial2.begin(9600, SERIAL_8E1, rx, tx)` | `uart_driver_install` + `uart_param_config` (`UART_PARITY_EVEN`), `uart_read_bytes` with a tick timeout |
| `millis()` | `esp_timer_get_time() / 1000` (or `xTaskGetTickCount`) |
| `delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| `pinMode` / `digitalWrite` | `gpio_config` / `gpio_set_level` |
| `EEPROM.read/write/commit` | `nvs_get_u8` / `nvs_set_u8` / `nvs_commit` |
| `WiFi.begin/status/RSSI/BSSID` | `esp_wifi_*` + `esp_netif`, connection state via an event group |
| `PubSubClient` | `esp_mqtt_client_*` (`mqtt_client.h`), LWT via `mqtt_cfg.session.last_will` |
| `WiFiClientSecure.setInsecure()` | `esp_mqtt` `broker.verification.crt_bundle_attach = esp_crt_bundle_attach` (proper verification, not insecure) |
| `ArduinoOTA` | `esp_https_ota` / `POST /ota/upload` + `esp_ota_*`, rollback enabled |
| `Serial.printf` | `ESP_LOGI/W/E` |
| `String` (2 uses) | `char[]` / `std::string` |

## Phases

1. **Scaffold** — IDF project builds and boots. *(done)*
2. **X10A read path** — `althermaserial` + `converters` + `labeldef` + `def/`;
   prove it by logging decoded values over USB serial with no WiFi involved.
   This is the highest-risk, highest-value step: verify against the real heat
   pump before anything else is built on top. *(code written, awaiting
   verification on the heat pump)*
3. **WiFi + MQTT publish** — `wifi.c`, `mqtt.c` (log mirror folded in); JSON ATTR
   payload byte-compatible with upstream so existing HA dashboards keep working.
   *(done, pending a broker to talk to)*
   Web UI + `settings.c` + OTA upload were pulled forward from phase 6 and
   landed here too, so configuration is already runtime rather than compile-time.
4. **HA discovery** — port `homeassistant.cpp`, verify entity IDs match
   upstream's (`sensor.espaltherma_*`) unless deliberately renamed.
5. **Control outputs** — thermostat, Smart Grid, safety relay. **DROPPED, not
   deferred.** This heat pump is controlled by an existing xComfort
   installation, so the thermostat contact is already driven. Wiring a second
   relay across the same input would put two systems in contention for one
   contact — a worse outcome than not having the feature. It also means the
   subscribe side of MQTT is not needed: this firmware is publish-only by
   design, which removes the entire class of "MQTT command moves a physical
   relay" failure. Revisit only if xComfort is removed from the loop.
6. **Runtime config + web UI + OTA** — *mostly landed early, in phase 3.* What
   is left: WiFi credentials and poll frequency into NVS (still `secrets.h` /
   compile-time), and the model-definition selection question below.

## Deliberate deviations from upstream

- **No ESP8266, no M5Stack/M5Unified screen support.** ESP32 only; the display
  and battery-reporting code is dropped rather than ported.
- **No blocking spin loops.** Upstream's `extraLoop()`/`waitLoop()` pattern
  (busy-waiting while pumping `client.loop()` and `ArduinoOTA.handle()`) is
  replaced by tasks; nothing needs pumping in IDF.
- **MQTT TLS verifies the broker** instead of `setInsecure()`.
- **Configuration is runtime, not compile-time.**

## Deviations taken while porting (phase 2)

Three places where the ported code does not read like upstream. All are marked
`DEVIATION` in `main/converters.cpp`.

1. **conv 211** — upstream writes `if (data == 0)`, comparing the *pointer* to
   null, so the `OFF` branch is unreachable and every value takes the numeric
   path. Intent was clearly `data[0] == 0`. Behaviour is kept identical
   (`data == NULL`) so readings still match upstream byte for byte; not reached
   by protocol S. **Not fixed** — that is a change to what the firmware reports,
   and it belongs in its own commit with a unit to verify against.
2. **`if (dblData != NAN)`** — always true, since NaN compares unequal to
   everything including itself. So upstream always formats the value and prints
   `nan` when a conversion produced one. Kept, comment added.
3. **`convertTable217` (conv 201/217)** — **fixed, deliberately.** Upstream
   indexes a 19-entry table with an unchecked byte and prints it through a
   non-literal format string; any value above 18 reads off the end of the array.
   This is directly in this unit's path (`PROTOCOL_S.h` maps `0x55` offset 0
   "Operation Mode" to convid 201), so the index is bounded and out-of-range
   states print as `State <n>`. An out-of-bounds read on the one register that
   reports faults was not worth preserving for fidelity.

## Target unit — what it changes

**Daikin Altherma LT split hydrobox EKHBH / EKHBX 008BA**, BA generation
(~2009-2011), R410A. Selected in `main/model_config.h`.

The BA generation speaks **protocol S**, not protocol I — upstream documents this
exact family in `doc/Daikin S protocol.md` (EKHBH016BA6WN, 2009, issue #46).
Three consequences for the port:

1. **Protocol S is the path that must work**, so port and verify it first. It is
   also the simpler of the two: 3-byte query, fixed reply lengths, registry ID in
   byte 0 of the reply (protocol I puts it in byte 1 and carries a length byte).
   Port I as well — it is a few lines — but do not let it drive the design.
2. **Only 4 registries and 25 values** (`0x50`, `0x53`, `0x54`, `0x55`). The
   `updateValues` / `getLabels` machinery still applies, but `registryIDs[32]`
   and the 128-label scratch arrays are wildly oversized for this unit; keep them
   generous anyway so a protocol-I machine still works.
3. **`Converter::RType` defaults to 802 (R32)** — wrong for this unit, which is
   **R410A = 801**. That default silently skews every pressure→temperature
   conversion at `0x50`. Set it explicitly when the converter is ported; do not
   rely on the upstream default.

**RESOLVED — it is the ROTEX variant.** The question was whether `PROTOCOL_S.h`
or `PROTOCOL_S_ROTEX.h` describes this machine; both speak protocol S and poll
the same registries but disagree about what `0x54` means. Decided from a real
reply, as planned:

```
0x54 -> 54 fc 18 | 28 1f | b8 1f | b8 1f | 34 25 | 01 00 | 20 00 ...
```

| plain `PROTOCOL_S.h` | | ROTEX | |
|---|---|---|---|
| Indoor suction air | 24.98 | Refrig. liquid side | 24.98 |
| Indoor heat exch. | 31.16 | Inlet water | 31.16 |
| Outdoor air | 31.72 | Outlet water | 31.72 |
| Outdoor heat exch. | 31.72 | D (unknown) | 31.72 |
| Discharge pipe | 74.41 | DHW tank | 37.20 |

The plain mapping claims a 74 C discharge pipe while `0x53` reports the
compressor off, invents an indoor-air sensor a hydrobox does not have, and puts
two unrelated sensors on the identical value. The ROTEX mapping gives
inlet/outlet water 0.56 K apart and a 37 C tank, and both water temperatures
move between polls. Corroborating: this unit has the 8-pin ROTEX-style X10A and
answers `0x50` with `0x15 0xEA`, matching that file's "0x50 not supported" note.

Two further findings from the live link:

- **Protocol S confirmed by sweep.** `POST /api/probe` asked both dialects; only
  the protocol-S registries ever replied.
- **`0x56` replies with 4 bytes**, CRC valid — so upstream's *code* is right and
  its `doc/Daikin S protocol.md` (which says 6) is wrong. Recorded in FSD §5.

## Open questions

- **Model definition selection**: keep compile-time `#include "def/<model>.h"`
  (simple, small binary) or embed several and select in NVS at runtime (needs a
  redesign of `labelDefs[]` from a static array to a parsed table)? Phase 2
  assumes compile-time; revisit at phase 6. Low stakes now that the unit is
  known and protocol S has one plausible definition (plus the Rotex variant).
- **Topic compatibility**: keep upstream's `espaltherma/*` topics verbatim (drop-in
  replacement for an existing HA setup) or namespace them?
