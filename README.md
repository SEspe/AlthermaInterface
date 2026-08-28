# AlthermaInterface

ESP32 firmware that reads a **Daikin Altherma** heat pump over its X10A service
connector and publishes every value to MQTT / Home Assistant — built on
**ESP-IDF v6.x**, no Arduino framework and no PlatformIO.

Status: **v1.0.0, running on a real machine.** Verified against a Daikin Altherma
LT split hydrobox (EKHBH/EKHBX 008BA, protocol S) — the X10A link has run for
hours with zero CRC errors, and the values reach Home Assistant with no YAML.

## What it does

- **Reads the X10A service port** — 9600 8E1, Daikin protocol **S** and **I**,
  CRC-checked, with the per-model value definitions from upstream.
- **Publishes to MQTT** in upstream ESPAltherma's exact payload format, so an
  existing Home Assistant setup needs no reconfiguration.
- **Home Assistant discovery** — entities appear automatically, with device
  classes and long-term statistics.
- **On-device web UI** — measurements, link diagnostics, configuration and OTA:
  - *Daikin Data* — every decoded value, refreshed live
  - *Debug* — X10A link health per registry, WiFi, MQTT activity counters, device
  - *Config* — WiFi (scan and pick a network), MQTT broker, X10A pins, repo
  - *OTA* — upload a `.bin`, or flash a GitHub release the device downloads itself
- **WiFi provisioning** — with nothing configured it raises an open access point
  named `AlthermaInterface` at `192.168.4.1` so the network can be chosen from a
  browser. Static IP supported, for routers that associate a client but never
  answer its DHCP request.
- **OTA with rollback** — dual app slots; an image that fails to boot is reverted
  automatically, so a bad upload cannot brick a unit sealed inside a heat pump.

**Read-only by design.** It publishes and never subscribes, and drives no GPIO,
so it cannot command the heat pump. See `FSD_AlthermaInterface.md` §2.

## A find, for anyone else on protocol S

A scan of all 256 registry IDs on this machine turned up **registry `0x5A`**,
which appears in no upstream definition file and in no published documentation.
Correlating it against a live heating cycle showed it is the **raw ADC readout**
of the sensors `0x54` reports converted — individual channels track inlet water,
outlet water and refrigerant temperature at r ≈ −0.93 to −0.99, bracketed by a
zero channel and a 1020 (10-bit full-scale) channel. Full method and data in
[`docs/REGISTER_0x5A.md`](docs/REGISTER_0x5A.md).

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

- `GPIO16` ← X10A **TX**, `GPIO15` → X10A **RX**, 9600 8E1 (`main/board_config.h`)
  — note this differs from upstream's 16/17 default
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

Then flash over USB, or upload `build/AlthermaInterface.bin` from the device's
OTA tab.

## License

MIT — see `LICENSE`, which carries both the upstream copyright and this one.
