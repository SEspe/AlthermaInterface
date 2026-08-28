# AlthermaInterface

ESP32 firmware that reads a **Daikin Altherma** heat pump over its X10A service
connector and publishes every value to MQTT / Home Assistant — built on
**ESP-IDF v6.x**, no Arduino framework and no PlatformIO.

Status: **scaffold**. The IDF project builds and boots; the functional modules
are being ported one at a time (see `docs/PORTING.md`).

## Credit — this is a derivative of ESPAltherma

The X10A protocol work, the registry/value conversion logic and the ~40 heat
pump definition files that make this possible are **not** original to this
project. They come from:

> **[raomin/ESPAltherma](https://github.com/raomin/ESPAltherma)** — © 2020 Raomin, MIT licensed.

That repository is the input to this one. Its protocol documentation
(`doc/Daikin I protocol.md`, `doc/Daikin S protocol.md`), its per-model value
definitions (`include/def/*.h`) and its converter table (`include/converters.h`)
are reused here with attribution; if you want the mature, widely-deployed,
Arduino/PlatformIO version — **use theirs**, not this one.

This project exists only to move that work onto native ESP-IDF, to match the
rest of this author's ESP32 firmware (see the sibling BirdBox project) and to
gain runtime configuration, a web UI and OTA with rollback.

Upstream reference clone (git-ignored, not part of this repo):

```sh
git clone https://github.com/raomin/ESPAltherma.git upstream-ESPAltherma
```

Pinned at upstream commit `4e518ec` (2026-07-24) for this port.

## Why ESP-IDF instead of the Arduino original

| | upstream ESPAltherma | AlthermaInterface |
|---|---|---|
| Framework | Arduino + PlatformIO | ESP-IDF v6.0.1, CMake |
| Config | compile-time `src/setup.h`, reflash to change | NVS + web UI (planned) |
| MQTT | PubSubClient | esp-mqtt (native, TLS via cert bundle) |
| OTA | ArduinoOTA | HTTPS/web OTA, dual slots + auto rollback |
| Loop | single `loop()`, `delay()` everywhere | FreeRTOS tasks, no blocking spin |

## Hardware

Same wiring as upstream — an ESP32 on the heat pump's X10A connector:

- `GPIO16` ← X10A **TX**, `GPIO17` → X10A **RX**, 9600 8E1 (`main/board_config.h`)
- optional dry contacts: thermostat, Smart Grid (SG1/SG2), safety relay
- **Beware:** X10A supplies 5 V; wiring mistakes can damage the heat pump's PCB.
  Read upstream's README wiring section before connecting anything.

## Build

ESP-IDF v6.0.1, target `esp32`:

```powershell
$py = "C:\Users\Stein\.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe"
$env:IDF_PATH = "D:\esp\v6.0.1\esp-idf"
& $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { $n=$matches[1]; $v=$matches[2].Replace('%PATH%',$env:PATH); Set-Item "env:$n" $v } }
& $py "$env:IDF_PATH\tools\idf.py" build
```

See `CLAUDE.md` for the full build/flash/verify workflow.

## License

MIT — see `LICENSE`, which carries both the upstream copyright and this one.
