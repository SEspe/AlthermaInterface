# FSD — AlthermaInterface

**Version:** 1.24
**Firmware:** 1.14.0
**Target:** ESP32 (ESP32-WROOM devkit, 4 MB flash), ESP-IDF v6.0.1
**Heat pump:** Daikin Altherma LT split hydrobox **EKHBH / EKHBX 008BA** —
**protocol S**, ROTEX value mapping

Functional Specification for AlthermaInterface — **the clean, current
specification**: what the firmware must do, stated in the present tense. This
document is authoritative for that; `docs/PORTING.md` covers *how the upstream
code maps onto it*.

It deliberately carries **no history**. The development record — what changed at
each version and why — is in
[`FSD_AlthermaInterface_CHANGELOG.md`](FSD_AlthermaInterface_CHANGELOG.md).
Read this file to build the thing; read that one before reopening a question
that was already settled, because much of what it records was established
against the real heat pump.

## 1. Purpose

Read every available operating value from a Daikin Altherma heat pump via its
X10A service connector and expose them to Home Assistant over MQTT.
Read-only: see §2 on why the control outputs are not ported. Same source
material as upstream ESPAltherma; different foundation, narrower scope.

## 2. Scope

**In scope:** ESP32 targets, X10A protocols I and S, MQTT publish + Home
Assistant discovery, runtime configuration, OTA.

**Out of scope:** ESP8266, M5Stack/M5StickC screens and battery telemetry,
Daikin models outside the upstream definition set.

**Also out of scope: controlling the heat pump.** The thermostat, Smart Grid
and safety-relay outputs are not ported. This machine is controlled by an
existing xComfort installation, so its thermostat contact is already driven;
a second relay across the same input would put two systems in contention. The
firmware is therefore **publish-only** - it subscribes to no MQTT topic and
drives no output. See docs/PORTING.md phase 5.

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

### 3.3 Onboard OLED (optional)
A 128×64 SSD1306 on I²C — **`SDA = GPIO5`**, **`SCL = GPIO4`**, address
**`0x3C`** — as fitted to the Lolin ESP32 boards this runs on. Pins in
`main/board_config.h`.

**The display is optional and detected, never configured.** `alt_display_init()`
probes the address once at boot; if nothing answers it logs the fact and every
later call is a no-op. One binary therefore serves a board with a panel and a
plain WROOM devkit without one, and there is no setting to get wrong.

The panel is **output only**. It shows what the device already publishes, so
the firmware remains publish-only (§2) and nothing on screen influences what is
polled or sent.

Layout, redrawn once per query cycle immediately after the derived values are
recomputed, so screen and published values come from one evaluation:

```
AlthermaInterface
192.168.10.40

Comp ON  power

In   32.3 C
Out  36.6 C
Tank 44.2 C
```

The compressor **source** is shown beside the state because the two sources
differ by tens of seconds on every edge (§6), so "ON" alone does not say how
fresh it is. A reading that is missing, not yet taken, or whose label has been
renamed shows as `--` rather than a stale or wrong number. Before the first
reading exists the screen carries a boot banner, so the panel says something
during the WiFi connect wait instead of looking dead.

I²C errors are logged once per outage, not once per cycle: a loose panel must
not drown the log the X10A link is diagnosed from.

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
both. **Settled on this unit: the code is right and the doc is wrong.** `0x56`
is polled by the ROTEX definition and answers in 4 bytes with a valid CRC.

Confirmed live: only protocol-S registries ever reply. A sweep of both dialects
(`POST /api/probe`) got answers on `0x53`/`0x54`/`0x55`/`0x56` and silence on
every protocol-I registry. `0x50` returns `0x15 0xEA` on this machine and is not
polled.

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

### Derived values

State the machine determines but does not report. `main/derived.c` computes
these once per poll cycle, between reading the registries and publishing, so
the web UI and MQTT always show one evaluation rather than two made moments
apart. A derived value carries no registry — `/api/values` marks it `calc` —
and is omitted entirely while it cannot be determined, exactly as an unread
label is.

**`Compressor numeric`** — the same state as `0` or `1`, for
`sensor.espaltherma_compressor_numeric`. It exists for long-term statistics,
which a binary sensor does not feed: the hourly mean of a 0/1 series is the
compressor's duty cycle. Both forms are written by one call in `derived.c`, so
they cannot disagree. No unit and no device class, `state_class: measurement`
set explicitly, and it is the only unquoted value in the `ATTR` payload.

**`Compressor`** — `ON` or `OFF`, from whichever of two sources can be trusted.
There is no compressor field to read: the 256-ID scan found five registries that
answer, and `0x53`'s only non-zero offsets in any capture are 0, 3 and 5. The
plain `PROTOCOL_S.h` map does carry `INV Comp. Frequency`, but that mapping was
rejected against real bytes — those fields belong to an outdoor-unit PCB this
service port does not reach.

**Preferred source: the outdoor unit's supply current**, read over HTTP from a
PowerMeter node every 5 s. `ON` at or above 2.0 A, `OFF` below 1.5 A
(hysteresis). The current steps within a second of the compressor starting or
stopping, so this source is accurate to about 5 s on both edges.

A change of state **wakes the poll loop immediately** rather than waiting for
the next cycle, because a state known within 5 s but published up to 30 s later
would throw most of the benefit away. That costs one extra query cycle per
transition.

The thresholds must clear **two** standby levels, not one: the outdoor unit
draws about 0.43 A once settled but about **0.99 A for some minutes after a
stop**, which is the crankcase heater. A threshold set just above the settled
figure latches on the post-stop one and never clears. Both are settings, since
the right values depend on the machine.

This source is used **only while it can be trusted**. It is refused when no host
is configured, the node is unreachable, the reading is older than 20 s, or the
current is outside 0–60 A. Refusal is not a guess: it hands the decision back to
the water delta.

**Fallback source: the water delta.** `ON` when the circulation pump is running
*and* outlet water is at least 2.0 K above inlet water; `OFF` below 1.2 K
(hysteresis), and `OFF` whenever the pump is stopped. Always available, needing
nothing beyond X10A, but **systematically late** — measured against the current
step, it declared a start 39 s late and a stop 40 s late. The lag is thermal on
the OFF edge and cannot be tuned away.

**`Compressor source`** — `power` or `delta`, published as a diagnostic, and
empty while the state is undetermined. The two sources differ by tens of seconds
on every edge, so a record made under one is not directly comparable with a
record made under the other; a silent fallback would make the history look
consistent when it is not.

The fallback rule rests on three captures in `captures/` — a forced DHW cycle, an
overnight run and a space-heating run. With the pump circulating, the water
delta is sharply **bimodal**: 0.1–0.9 K with the compressor idle, 3–7 K with it
running, and 8 of 426 pump-on samples anywhere between 1 and 3 K, all of them
mid-transition. The refrigerant liquid side agrees independently — about 5–6 K
below inlet water when idle, level with it when running — but is **not** used
in the rule: it lags by minutes after a stop, and its spread during genuine
compressor operation (−4.6 to +1.7 K against inlet) would produce false
negatives if it were ANDed in. The delta responds immediately.

Two limits are inherent. A **space-heating backup heater**, if this unit has one
and if it reports on `0x53` offset 3, would raise the delta with no compressor
running and read as `ON`; the offset-3 element observed so far heats the DHW
tank with no circulation at all, which this rule cannot see and does not claim
to. And at a 60 s poll interval a **short cycle can be missed entirely** — the
same resolution caveat that makes these captures unsuitable for duty-cycle or
runtime totals.

The inputs are located by registry and payload offset, not by label — labels are
user-facing text and keying on them is the trap `docs/REGISTER_0x53.md`
describes. Because offsets alone would silently mean something else under a
different definition file, each label is checked once as a **guard**: a mismatch
disables the sensor with an error in the log rather than publishing a confident
lie.

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

The **compressor power source** is configured the same way: the PowerMeter host,
which channel of it to read, and the two current thresholds. An empty host
disables the source entirely, which is the shipped default — a device that has
never been told about a PowerMeter behaves exactly as one that predates the
feature. The thresholds are settings rather than constants because the right
values depend on the machine's standby draw and its minimum modulation, neither
of which this firmware can discover for itself. `POST /api/config` rejects a
pair where the off threshold is not below the on threshold, since that is not
hysteresis but a rule that can never settle.

Still compile-time, to be moved later: WiFi credentials (in `secrets.h`), poll
frequency, output pin roles, and the model definition selection (see
`docs/PORTING.md`, open questions).

## 10. Web UI

Served by the device itself on port 80 (`main/web_server.c`), four tabs:

- **Daikin Data** — every label that has been read at least once, with its
  registry and current value; refreshes every 5 s.
- **Debug** — X10A link diagnostics per registry, WiFi, MQTT counters, device
  and ESP32 internals, and the **compressor probe** (below).

  The probe block reports whether a PowerMeter host is configured, the host and
  channel, the state that source currently gives, the last reading with its
  age, the configured thresholds, poll/ok/fail counts, the reason the last poll
  failed, the last HTTP status, and — separately — **which source actually
  decided the published compressor state**.

  It exists because **the fallback to the water delta is silent by design**.
  The compressor sensor keeps working when the probe fails, so nothing else on
  the device shows that the timing has quietly reverted to a source measured
  40 s late on every edge. The distinct failure reasons matter for the same
  reason: an unreachable node, a node answering with a non-200 status, and a
  mistyped channel label are indistinguishable from the sensor, need entirely
  different fixes, and are told apart here.
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
