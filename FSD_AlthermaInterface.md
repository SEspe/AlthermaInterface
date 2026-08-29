# FSD — AlthermaInterface

**Version:** 1.15
**Firmware:** 1.8.0
**Target:** ESP32 (ESP32-WROOM devkit, 4 MB flash), ESP-IDF v6.0.1
**Heat pump:** Daikin Altherma LT split hydrobox **EKHBH / EKHBX 008BA** —
**protocol S**, ROTEX value mapping

Functional Specification + change record for AlthermaInterface. This document
is authoritative for *what the firmware must do*; `docs/PORTING.md` covers *how
the upstream code maps onto it*.

## Changelog

- v1.15 — **Identify `0x5A` offset 8: a buffered water-circuit sensor
  (firmware 1.8.0), §6, §7.** The last unidentified live channel on the
  undocumented register. `5A@8` and `5A@10` had tracked each other at r =
  0.977 across every recorded condition, and neither space heating nor a DHW
  cycle separated them — the doc predicted a defrost or cooling cycle would be
  needed.

  What separated them was neither: a **pump stop**. With circulation halted
  and the tank booster running, outlet water fell 51 → 32 °C in 90 s while
  `5A@8` held ~51 °C, decaying over three minutes to meet it — 177 counts
  apart at the peak, r falling from 0.996 to 0.635. A 2×2 control over pump,
  DHW priority and heater state shows the divergence follows the **pump**
  (−82.9 counts mean with flow stopped) and not the heater (−2.6 while
  circulating) or DHW priority (+5.9).

  So `5A@8` is a water-circuit sensor with a long thermal time constant — a
  vessel or body holding water rather than a pipe, which is why it is
  indistinguishable from outlet water whenever anything is flowing. The backup
  heater vessel fits the behaviour; that location remains inference, the lag
  is measured. Label changed from `ADC water circuit (unassigned)` to `ADC
  water circuit (buffered)`, which **renames the Home Assistant entity** — the
  old one is orphaned and should be deleted by hand. Full reasoning in
  `docs/REGISTER_0x5A.md`.

  **Also, and found while making this release:** a version bump alone never
  reached the app descriptor. `PROJECT_VER` is read from `main/version.h` by
  `file(STRINGS)`, which runs at CMake *configure* time, and nothing declared
  that file as a configure dependency — so editing it rebuilt the app while
  CMake kept the previous value. This build first came out as **1.8.0** in
  `/api/status` and **1.7.1** in the descriptor that the web installer and the
  boot banner read: exactly the split v1.14 set out to remove, reintroduced
  one layer down. `CMAKE_CONFIGURE_DEPENDS` now names `main/version.h`, so
  touching it forces a reconfigure. Verified by rebuilding and reading the
  version back out of the descriptor.

- v1.14 — **One version number, from one place (firmware 1.7.1), §9, §10.**
  The web installer reported **1.6.3** for a build the device itself reported as
  **1.7.0**. Both were reading real values — just different ones.

  An ESP-IDF image carries a version in its **app descriptor**, which is what
  esp-web-tools and the boot banner read. Left alone, `PROJECT_VER` comes from
  `git describe`, which answers differently depending on where and when the build
  happened: `v1.6.3-2-ge805d10` on a machine whose newest tag is older than the
  code, a bare commit hash in CI (the checkout is shallow and carries no tags),
  and something else again once a tag is pushed. Meanwhile `/api/status` reports
  `FIRMWARE_VERSION` from `main/version.h`. The two had never matched; nothing
  had made it visible until the web installer put a stale number in front of
  someone.

  The top-level `CMakeLists.txt` now parses `FIRMWARE_VERSION` out of
  `main/version.h` and sets `PROJECT_VER` from it, before `project()`. The
  descriptor, the boot banner, the web installer and `/api/status` all report the
  same string, and none of them depend on tags, checkout depth, or whether a
  build happened before or after a release was cut. A missing or unreadable
  `FIRMWARE_VERSION` is a hard CMake error rather than a silent fallback.

  `main/version.h` was already the single source of truth by the release
  contract; now the build honours it.

- v1.13 — **Never sleep the radio without a lease (firmware 1.7.0), §4,
  §3.2.** A newly provisioned board never got a DHCP address, while every other
  device on the same network leased normally.

  **The investigation produced a wrong answer first, and it is recorded here
  because the wrong answer was convincing.** Comparing against the sibling
  BirdBox project showed BirdBox sets `WIFI_PS_NONE` explicitly, its header
  recording a *"v1.32 lesson: modem-sleep latency ruins HTTP"*, while
  AlthermaInterface ran `WIFI_PS_MIN_MODEM`. Modem sleep parks the radio between
  DTIM beacons, so a DHCP `OFFER` or `ACK` arriving in that window is missed — a
  real, documented ESP32 failure that depends on how the access point buffers
  frames, which would neatly explain why only this device is affected.

  **Hardware disproved it.** With power save verifiably off (`wifi:Set ps
  type: 0` in the boot log) a freshly provisioned board still sits associated
  with `dhcpc status 1` and never receives a lease. Modem sleep was not the
  cause, and the older note in `wifi.c` blaming the access point may well have
  been right all along. Current suspicion is that the network refuses *new*
  clients — the devices that work already hold leases — which the DHCP pool size
  and lease table would settle. **Unresolved.**

  The change below is kept anyway, on its own merits: the code was genuinely
  wrong to allow modem sleep without an address, whatever else is also wrong.

  The fix is stated as an invariant rather than a startup special case, because
  a startup-only fix leaves a trap: a unit that leases an address, enables modem
  sleep, then **misses a lease renewal** at T1 loses its IP and retries the
  DISCOVER with modem sleep still on — the very thing that loses the reply. It
  would sit associated and address-less indefinitely, and the existing retry
  logic could never escape, because the cause is still enabled.

  **Modem sleep may only ever be on while the station holds an IP.** Enforced at
  every transition:

  | Event | Action |
  |---|---|
  | `STA_CONNECTED` (associated, not yet leased) | power save off |
  | `STA_GOT_IP` | apply the configured level |
  | `STA_LOST_IP` (lease expired or released) | power save off |
  | `STA_DISCONNECTED` | power save off |
  | stall watchdog, associated with no IP | power save off |

  `IP_EVENT_STA_LOST_IP` is newly registered; only `GOT_IP` was handled before.
  `alt_power_ps_off()` checks the current mode first and logs only on a real
  transition, so a flapping link does not fill the log.

  **Level 0 "Off" now means `WIFI_PS_NONE`.** It previously set
  `WIFI_PS_MIN_MODEM`, so a profile named "Off" was still parking the radio.
  Levels 1 and 2 keep modem sleep, which is part of what they buy — but only
  ever while a lease is held.

  Side effect worth having: at level 0 the web UI loses the DTIM-interval
  latency modem sleep adds to every HTTP exchange.

- v1.12 — **Fix: a board flashed from a public release could never finish
  booting (firmware 1.6.3), §4, §6, §10.** Found by flashing the second board and
  joining the `AlthermaInterface` provisioning AP, which answered nothing at
  192.168.4.1. The AP itself was fine; the device behind it was boot-looping.

  A release binary is built by CI, which has no `secrets.h`, so on a device with
  empty NVS the **broker URI is the empty string**. `esp_mqtt_client_set_uri()`
  rejects that — reporting `Memory exhausted`, which points nowhere near the
  cause — and the `ESP_ERROR_CHECK` around `alt_mqtt_start()` turned it into
  `abort()`. Every freshly flashed board died before its provisioning page could
  be served, which is precisely the path a new user takes.

  Two changes:

  1. **An unconfigured broker is no longer an error.** `alt_mqtt_start()` returns
     early with a warning when the URI is empty, because not being configured yet
     is the normal first-boot state rather than a failure. The caller logs
     instead of aborting: a broker that is unset or unreachable must never cost
     the device the web UI, which is how it gets configured in the first place.
  2. **The web server now starts before the 30 s connect wait.** On an
     unprovisioned board the SoftAP is already serving and the config page is the
     entire point of it; blocking behind a station connection that cannot happen
     left 192.168.4.1 dead for the full timeout even once the abort was fixed.

  This is the second `ESP_ERROR_CHECK`-on-a-non-critical-subsystem defect in two
  releases (see v1.11). The pattern is now understood as the hazard it is on a
  device whose recovery path is a serial cable inside a heat pump.

- v1.11 — **Fix a boot loop caused by running out of HTTP route slots; re-arm
  OTA rollback (firmware 1.6.2), §9, §10.** Firmware 1.6.1 added two routes to a
  server configured for exactly 12, with 12 already registered. `httpd` returned
  `ESP_ERR_HTTPD_HANDLERS_FULL`, `ESP_ERROR_CHECK` turned that into `abort()`,
  and a deployed unit boot-looped five times before it was recovered over
  serial. WiFi and MQTT both connected *before* the abort, so from the network
  it looked like a link fault rather than a crash.

  Three changes, because the bug needed one and the damage needed two:

  1. `cfg.max_uri_handlers` raised from 12 to 24. The comment beside it claimed
     headroom and claimed overflow would "404 silently"; both were false.
  2. **Route registration is no longer fatal.** A route that fails to register
     now logs an error and the firmware carries on. A missing endpoint costs one
     feature; aborting costs the whole device, on a board where recovery means a
     serial cable inside a heat pump.
  3. **`esp_ota_mark_app_valid_cancel_rollback()` moved to after all
     initialisation**, where it belongs. It had been called immediately after
     `nvs_flash_init()`, so 1.6.1 marked itself valid milliseconds before
     aborting, and the bootloader dutifully kept re-launching the broken image.
     With the call at the end of setup, this exact failure would have rolled
     back on its own.

     It is deliberately **not** tied to a successful heat pump query, as an
     earlier note in `main.c` suggested: the pump can be powered down for
     service, and rolling firmware back because the machine is off is a worse
     failure than the one it prevents.

- v1.10 — **Debug tab gains Refresh, Reboot and an ESP32 internals section;
  heap diagnostics reach MQTT (firmware 1.6.1), §7, §10.** The page already polls every 5 s, but **Refresh** forces an immediate
  read for when something has just been changed at the heat pump and the next
  tick is too long to wait; it stamps the time it last updated so a stalled page
  is obvious. **Reboot** restarts the device over a new `POST /api/reboot`,
  which answers before `reboot_task` fires so the browser gets a reply rather
  than a dropped connection. It asks for confirmation first, since it drops the
  link for a few seconds, and the page recovers on its own once the device
  answers again. Until now a restart meant a power cycle or saving an unchanged
  setting on the Config tab.

  An **ESP32 INTERNALS** table reports chip model, revision and cores, flash
  size, MAC, ESP-IDF version, heap total, task count and the running power
  settings. Its static half comes from a new `GET /api/internals` fetched **once**
  at page load rather than on the 5 s tick, because a second periodic request
  would work against the power profiles for values that cannot change.

  Three diagnostics also join the MQTT ATTR payload and Home Assistant
  discovery: **MinFreeMem**, the low-water mark since boot, which is what reveals
  a slow leak long before the current free figure looks wrong; **MaxFreeBlock**,
  which reveals fragmentation that a plain free-heap number hides; and
  **Uptime**. Existing keys are unchanged, so dashboards keep working.

  **No die temperature is reported anywhere.** The ESP32 classic has no
  supported internal temperature sensor — the undocumented ROM
  `temprature_sens_read()` is uncalibrated and reads a near-constant value on
  most chips. Publishing it would put an invented measurement into Home
  Assistant's long-term statistics, so the UI says it is unavailable instead.

- v1.9 — **Configurable power profiles (firmware 1.6.0), §3.2, §10.** The board
  is fed from the heat pump's internal 5 V regulator, which was never sized for
  an ESP32, so the Config tab now selects how much load it puts on that rail.
  Three cumulative levels: **0 Off** (160 MHz, 20 dBm — the behaviour of every
  earlier release), **1 Balanced** (160 MHz, 13 dBm) and **2 Low** (80 MHz,
  13 dBm). The names, descriptions, frequencies and transmit powers all come
  from one table in `main/power.c`, so the web UI cannot drift out of step with
  what the firmware does, and `/api/status` reports the running `power`,
  `cpuMhz` and `txDbm` so a level can be verified rather than assumed.

  Transmit power is the lever that matters: WiFi bursts of a few hundred
  milliamps against a ~40 mA baseline are what stresses a small regulator, so
  cutting 20 dBm to 13 dBm buys more than shaving idle draw does. Savings are
  datasheet estimates, not measurements — the firmware cannot see its own supply
  current.

  **The default stays 0.** An OTA that silently reduced transmit power could put
  a unit with a marginal link out of reach inside a heat pump enclosure,
  recoverable only over serial or SoftAP, so raising it is an explicit choice
  and the UI says to do it one step at a time.

  The UI also says what to watch, because the obvious answer is wrong: **RSSI
  cannot show the effect of reducing transmit power**, being the signal arriving
  *from* the access point. The counters that do move are `disconnects`,
  `connects` and `pubFail`. RSSI remains useful *beforehand* as a measure of
  margin, since path loss is roughly symmetric.

  `CONFIG_PM_ENABLE` is now set, since `esp_pm_configure()` is the only
  supported way to change CPU frequency at runtime. Every profile sets
  `min_freq == max_freq`, so **no dynamic scaling actually occurs**: 80, 160 and
  240 MHz are all PLL-derived and leave APB at 80 MHz, which is what keeps the
  X10A UART's 9600 8E1 divisor correct. Automatic light sleep — the largest
  saving available — is deliberately **not** included: it would drop APB to the
  XTAL frequency and requires moving the UART to `UART_SCLK_REF_TICK` plus a
  power-management lock held across each query, which touches the field-proven
  serial path and needs its own verification against the real heat pump.

- v1.8 — **`0x53` offset 3 confirmed as the electric heater contactor
  (firmware 1.5.0), §6.** The bit upstream labels `External heater?` — with its
  own question mark — was verified directly: the owner heard the contactor
  engage three times while three concurrent captures recorded payload offset 3
  flipping `0x00` → `0x01` in exactly three clusters, with the CRC dropping one
  count as a single byte rose. It is renamed `Electric heater contactor`, which
  changes its Home Assistant entity from
  `binary_sensor.espaltherma_external_heater` to
  `binary_sensor.espaltherma_electric_heater_contactor`; the old entity goes
  unavailable and must be deleted once. The bit is worth surfacing because
  resistive backup heat is the expensive operating mode. New
  `docs/REGISTER_0x53.md` records this and two further confirmations made the same
  day, each by a deliberate physical action: **offset 0 `Circulation pump`**
  (stopping the room thermostat drove it 1 → 0) and **offset 5 `Priority to
  domestic water`**, which answers the question that started the investigation —
  the **3-way valve** was watched physically diverting as the bit went 0 → 1 on a
  forced DHW call, with outlet water rising 6.9 K within a minute. Three of
  upstream's four `0x53` labels are now verified. `Operation Mode` is recorded as
  unusable for this: it reads `Heating` through a full DHW cycle and even while
  the machine sits idle. The document also carries two warnings: a brief contactor pulse produces no
  measurable water temperature change, so absence of one proves nothing; and
  15 s polling cannot count contactor events or support duty-cycle figures.

- v1.7 — **Repository links in the OTA tab (firmware 1.4.0), §10.** The OTA tab
  links to the configured GitHub repository and to its releases page, so the
  source and the changelog are one click from the device. The links are built
  from the configured `owner/name` rather than hard-coded, so a fork points at
  its own project, and they degrade to "(none configured)" when no repository is
  set.

- v1.6 — **Strapping-pin guidance in the Config tab (firmware 1.3.1), §3.1.**
  The RX field is now marked *avoid strapping pins for safe boot*, and the hint
  explains the part that is not obvious from the word: **what matters is which
  side drives the pin.** TX is safe on a strapping pin because the device drives
  it and an idle UART line sits high — which is why GPIO15 is the default TX. RX
  is the risk, because the heat pump drives it and its level during a reset is
  outside our control. GPIO12 is called out separately: held high at reset it
  selects 1.8 V flash and a 3.3 V board will not boot at all, so its dropdown
  entry reads `strapping - avoid`.
  Refined: at reset the ESP32 drives nothing, since every GPIO is high-impedance
  until firmware configures the UART. A TX pin is safe because MTDO has an
  internal pull-up and the far end is a high-impedance receiver input, not
  because the device is driving it. The general rule is to avoid a strapping pin
  wherever something external could impose a level while the chip boots.

- v1.5 — **`0x5A` offset 12 identified as an unconnected input (firmware
  1.3.0), §6.** Three hypotheses tested against the overnight capture and all
  excluded: not a temperature (inert across a 25 K swing, and 9 counts sits on
  the bottom rail where the curve implies hundreds), not outdoor air (no
  overnight drift; with outside measured at 13.5 °C the curve implies ~680
  counts, and this is the hydrobox — the outdoor sensor belongs to the outdoor
  unit), and not power or load (mean 8.6 with the compressor running against 8.9
  idle, where `5A@8` moved 530 → 301 across the same split).
  A few counts of noise around zero, unmoved by anything, reads as an
  **unconnected ADC input** — most likely for optional hardware this unit does
  not have, of the kind the ROTEX definition already carries entries for.
  Labels updated: offset 12 becomes `ADC unused input`, and offset 8 becomes
  `ADC water circuit (unassigned)`, since it shadows inlet water within a couple
  of counts and swings with the compressor but has not been separated from
  offset 10.

- v1.4 — **Reading / Corresponds to / Factor as columns; estimates for the
  unidentified channels (firmware 1.2.0), §6, §10.** The Daikin Data table gains
  three columns instead of an annotation crammed into one cell.
  The two unidentified `0x5A` channels now get an **estimated** temperature,
  interpolated between the channels whose sensor is known and marked `~n °C est.`
  so it cannot be mistaken for a reading. Two rules keep the estimate honest:
  it **never extrapolates** — a count outside the basis range shows blank rather
  than a fabricated number — and the **DHW tank is excluded from the basis**,
  because its count rises with temperature while every other channel's falls, so
  it is a different sensor characteristic and mixing it in would produce
  nonsense.

- v1.3 — **ADC channels show the temperature they correspond to (firmware
  1.1.1), §6, §10.** Each identified `0x5A` channel now displays the matching
  `0x54` reading beside its count, plus a counts-per-degree ratio.
  The temperature is read from `0x54`, **not derived from the count**: the
  thermistor curve has not been characterised, and converting the count would
  present a guess as a measurement.
  The ratio is likewise marked as an indicator only — an NTC is not linear, so
  counts-per-degree drifts across the range. Its use is comparative: a channel
  whose ratio wanders away from its neighbours is the tell for a failing sensor.

- v1.2 — **Registry `0x5A` channels identified and named (firmware 1.1.0), §6.**
  An overnight capture across a **DHW cycle** — 471 samples, tank driven
  35.8 → 46.3 °C while outlet water spiked to 55 °C — supplied the independent
  movement that a space-heating run could not. Four channels are now named:

  | offset | channel | r |
  |---|---|---|
  | 2 | DHW tank | −0.992 |
  | 4 | inlet water | −0.999 |
  | 6 | refrigerant liquid side | −1.000 |
  | 10 | outlet water | −0.999 |

  plus the zero and full-scale references at 0 and 14. `5A@2` is the decisive
  result: it sat flat through hours of space heating and moved only with the
  tank, and that selectivity is what identifies it — during the DHW cycle
  everything warmed together (tank vs outlet r = 0.583), so correlation alone
  would prove little.
  The channels are now read with conversion 151, **unscaled**, because they are
  ADC counts. The earlier ÷10 made the first idle sample look like plausible
  temperatures; that was a coincidence of range, not meaning.
  **This changes MQTT payload keys and therefore Home Assistant entity IDs**:
  the eight `Probe 5A at N` entities are replaced by named `ADC …` ones.
  Still open: `5A@8` has not been separated from `5A@10` (r = 0.977), and
  `5A@12` is inert across a 25 K swing so it is not a temperature.

- v1.1 — **X10A pins become dropdowns; CI (firmware 1.0.1), §3.1, §10.** The
  Config tab offers only GPIOs that exist and can do the job — GPIO6-11 (SPI
  flash) omitted entirely, GPIO34-39 offered for RX only since they are
  input-only — with strapping pins and the console UART pair annotated rather
  than hidden. RX and TX are compared on change and again on save. The
  server-side check also rejects GPIO20, 24 and 28-31, which do not exist on the
  ESP32 and which the old 0-39 range test accepted.
  Adds `.github/workflows/ci.yml`: a build plus checks that secrets are not
  committed, that the local-only documents are not published, that
  `main/version.h` and this header agree, that the selected definition file
  exists, and that README links resolve. Every check fails closed. The version
  check caught a real drift on its first run — this entry.
  Also adds the browser-based web flasher (`docs/webflash/`) published to GitHub
  Pages, for the initial USB flash of a blank board.

- v1.0 — **First release (firmware 1.0.0).** The port is functionally complete
  and running on the reference unit: X10A read path, WiFi with provisioning and
  a SoftAP fallback, MQTT publishing, Home Assistant discovery, the four-tab web
  UI, and OTA both by upload and from a GitHub release.
  Verified against the real heat pump rather than in principle — hours of
  polling with zero CRC failures, values live in Home Assistant, and OTA proven
  ping-ponging between both app slots.
  Scope is settled: read-only, no control outputs (§2); protocol S with the
  ROTEX value mapping; plus registry `0x5A`, which this project identified and
  documented.

- v0.13 — **WiFi provisioning (firmware 0.11.0), §3, §8, §10.** Credentials and
  IP mode move to NVS, configurable from the Config tab: scan, pick a network,
  password, DHCP or static. Precedence is NVS, then `secrets.h`, then a SoftAP —
  which is what carried the deployed unit through the change without it dropping
  off the network. With nothing configured, an open AP named `AlthermaInterface`
  at `192.168.4.1` serves the same page so a blank board can be pointed at a
  network from a browser; APSTA, so scanning works while the AP is serving.
  Two guards, because the unit has no USB attached: a stored static mode with no
  address falls back to DHCP rather than booting unreachable, and the UI states
  that this AP does not answer DHCP.

- v0.12 — **Configurable X10A pins, GitHub-release OTA, Debug tab (firmware
  0.10.0), §3.1, §9, §10.** RX/TX become settings with validation (GPIO6-11
  refused as SPI flash, TX refused on input-only 34-39). The OTA tab gains
  flashing straight from a GitHub release, downloaded by the device over HTTPS
  because the asset host sends no CORS header. The WiFi tab becomes Debug and
  gains MQTT activity counters — published, failures, connects, disconnects,
  time since last publish — which separate "connected but silent" from "cannot
  connect".

- v0.11 — **Bring-up debug commands removed (firmware 0.9.0), §10.** The
  protocol sweep, 256-ID register scan and on-demand RX check had done their job
  and are gone from a firmware that lives inside a heat pump. The passive link
  table stays.

- v0.10 — **Control outputs dropped from scope (firmware 0.8.0), §1, §2.**
  Phase 5 is not deferred, it is cancelled: this machine is controlled by an
  existing xComfort installation, so its thermostat contact is already driven
  and a second relay across the same input would put two systems in contention.
  The firmware is therefore **publish-only** — it subscribes to no MQTT topic
  and drives no GPIO, which removes the whole class of "an MQTT message moves a
  physical relay" failure. Revisit only if xComfort leaves the loop.

- v0.9 — **Home Assistant discovery (firmware 0.8.0), §7, §10.** Phase 4.
  `main/homeassistant.cpp` publishes one retained device-discovery payload on
  every MQTT connect; entities appear with no YAML. Device identifiers, entity
  naming and the state topic are upstream's verbatim, so a machine that once ran
  upstream's firmware keeps its HA device and history.
  Two forced deviations: esp-mqtt has no streaming publish, so the payload is
  materialised and the MQTT buffer raised to 16 KB; and labels are read through
  the converters C facade, because the definition headers *define* `labelDefs[]`
  and including one from a second translation unit is a duplicate-symbol link
  error.
  Fixes a real gap rather than porting it: upstream infers device class from
  `dataType`, which every protocol-S definition leaves `-1`, so every
  temperature would have reached HA unitless and without statistics.
  Empty entity keys are skipped and logged — upstream's `"????"` label would
  otherwise emit a nameless component with `uniq_id` of just `espaltherma_`.

- v0.8 — **Full register scan; 0x5A found (firmware 0.6.0-0.7.0), §5, §6.**
  `POST /api/scan` walks all 256 registry IDs and classifies each. On this unit:
  5 ok, 243 `0x15 0xEA` "not implemented", 0 bad CRC, 8 silent.
  **`0x5A` answers with 18 bytes and a valid CRC and appears in no upstream
  definition file, no upstream document, and nothing found on GitHub or the
  wider web.** Six of its eight 16-bit channels drift continuously, so it
  carries live data. `main/def/EKHBH008BA.h` — our own definition, derived from
  `PROTOCOL_S_ROTEX.h` — polls it as eight probes. Full analysis, including the
  raw-ADC hypothesis and how to test it, in `docs/REGISTER_0x5A.md`.
  The first scan attempt panicked the board: the scan task's 4 KB stack could
  not hold the query function's buffers plus the MQTT log hook's 256-byte line
  buffer. Raised to 8 KB, and `/api/status` now reports `esp_reset_reason()` so
  a crash is visible rather than inferred from a low uptime.

- v0.7 — **Live on the heat pump; switched to the ROTEX mapping (firmware
  0.5.0), §5, §6.** The X10A link works: `0x53`/`0x54`/`0x55`/`0x56` all reply
  with valid CRCs and 18 of 18 labels decode.
  `main/model_config.h` now selects `def/PROTOCOL_S_ROTEX.h` instead of
  `def/PROTOCOL_S.h`. Both speak protocol S and poll the same registries; they
  disagree on what `0x54` means, and the plain mapping was wrong for this
  machine — it reported a 74 C discharge pipe with the compressor off, an
  indoor-air sensor a hydrobox does not have, and two unrelated sensors reading
  identically. The ROTEX mapping gives inlet 29.95 / outlet 30.41 / DHW tank
  37.20 C, values that move between polls. Full byte-level comparison in
  `docs/PORTING.md`; the open question from v0.2 is closed.
  Also settles the `0x56` reply-length discrepancy in favour of upstream's code
  over its documentation.

- v0.6 — **X10A link diagnostics (firmware 0.4.0-0.4.2), §10.** The unit runs on
  X10A power inside the enclosure with no USB, so the serial log is unavailable
  where faults happen. Per-registry outcome, byte count, ok/fail tallies and raw
  hex are recorded and served at `GET /api/x10a`; `POST /api/probe` sweeps both
  protocols; `POST /api/rxcheck` re-reads the RX pin level on demand, reported
  with when it was sampled so a stale reading cannot mislead. These found the
  fault: RX idle LOW meant the wire was not on the pump's TX, and once corrected,
  silence meant our TX was not connected at all.

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
