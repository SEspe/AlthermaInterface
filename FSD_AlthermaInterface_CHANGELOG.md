# FSD changelog — AlthermaInterface

The development record for `FSD_AlthermaInterface.md`. Every change to the
specification lands here, newest first.

The two files have different jobs. **`FSD_AlthermaInterface.md` is the clean
current specification**: what the firmware must do, in the present tense, with
no history in it. **This file is how it got that way** — what changed at each
version, and more importantly *why*, so a decision that looks arbitrary later
can be traced back to the evidence that forced it.

Read the FSD to build the thing. Read this when you want to know why it says
what it says, or before reopening a question that was already settled — much of
what is recorded here was established against the real heat pump and is
expensive to rediscover.

Entry format: `- vX.Y — **Title (firmware A.B.C), §section.** what + why + how`.
Per the release contract in `CLAUDE.md`, a functional change is not done until
the code, the version bump in `main/version.h`, and an entry here land together.

---

- v1.22 — **Onboard OLED status page (firmware 1.13.0), §3.3.** The Lolin ESP32
  boards this runs on carry a 128×64 SSD1306. It now shows the IP, compressor
  state and source, inlet and outlet water, and the DHW tank — the values you
  want standing at the machine with no phone in hand. Output only; the firmware
  stays publish-only and nothing on screen changes what is polled or sent.

  **Detected, not configured.** `alt_display_init()` probes 0x3C once at boot
  and disables itself silently if nothing answers, so one binary serves a board
  with a panel and a plain WROOM devkit without one. A settings flag would have
  been one more thing to get wrong for no gain.

  **The pin map was established empirically, and it mattered.** A sweep of the
  pairs that boards with onboard OLEDs use found the panel at **SDA=GPIO5,
  SCL=GPIO4** — the Lolin map, clear of X10A on 15/16. Had it been the
  Heltec/TTGO variant instead, the panel's RESET sits on **GPIO16**, which is
  this project's X10A RX, and the display and the heat pump link could not have
  coexisted without rewiring. That is not a fact to take from a datasheet: three
  board families share the "ESP32 with OLED" description and disagree on every
  pin.

  **Two diagnostic lessons, both expensive.** First, the initial dev board's
  panel is dead in a way that looks alive: its controller ACKs every I²C write,
  including charge-pump-on and display-on, while the glass never lights — not
  even for `0xA5`, which lights all pixels from the controller with no RAM
  involved. A successful I²C transaction proves nothing about pixels. Second,
  an I²C probe reports success whenever SDA is pulled low in the ACK slot, so a
  bus stuck low ACKs *every* address and is indistinguishable from a device
  unless the whole range is scanned. Scan the range and check both lines idle
  high before trusting any hit.

  Rendering is verified against a known-good panel: init sequence, 5×7 font,
  horizontal-addressing flush and the page layout, all as shipped.

- v1.21 — **The PowerMeter host field accepts a URL (firmware 1.12.2), §8.**
  The field is labelled "PowerMeter host or IP" and the first thing typed into
  it was `http://192.168.10.238/` — which is what a browser puts on the
  clipboard, and an entirely reasonable reading of the label. The firmware built
  `http://http://192.168.10.238//api/values`, could not fetch it, and fell back
  to the water delta.

  The fallback worked exactly as designed — the source was refused rather than
  guessed at, and `Compressor source` reported `delta` so the situation was
  visible rather than silent. But a field that rejects the obvious input is the
  field's fault. `alt_settings_set_powermeter()` now strips a `http://` or
  `https://` prefix and everything from the first slash, storing the bare
  `host[:port]` the URL builder expects. Normalised on save rather than on use,
  so `GET /api/config` and the Config tab show what was actually kept.

- v1.20 — **Compressor state from the outdoor unit's current, with the water
  delta as fallback (firmware 1.12.1), §6, §8.** The derived compressor sensor
  was never wrong about *state*, but it was always late about *timing*, and on
  2026-09-15 that was measured against ground truth for the first time rather
  than inferred.

  A PowerMeter node on the outdoor unit's supply gave the reference. Against
  the current step: the water delta declared a start **39 s late** and a stop
  **40 s late**. Previous estimates of the OFF lag (~24 s) had come from burst
  sampling of the thermal decay alone and omitted the poll quantisation on top
  of it.

  So the outdoor current becomes the preferred source, polled every 5 s, `ON`
  at or above 2.0 A and `OFF` below 1.5 A. A change of state now **wakes the
  poll loop immediately** — reusing the notification burst mode already had —
  because a state known within 5 s but published up to 30 s later would discard
  most of the benefit. That costs one extra query cycle per transition, a
  handful an hour against the 120 already performed.

  **The thresholds are measured, and the first ones proposed were wrong.** The
  outdoor unit has *two* standby levels: 0.43 A once settled, and **0.99 A for
  some minutes after a stop**, which is the crankcase heater. An initial
  proposal of 0.8 A on / 0.6 A off would have latched on that post-stop level
  and never cleared, reporting the compressor permanently ON. Running current
  over a full cycle was 4.1–7.7 A, so 2.0/1.5 A clears both standby levels with
  room. Unresolved: minimum modulation may be near 1 A, which would fall inside
  the post-stop band and be unresolvable by current alone — that figure
  predates the repair of the outdoor CT and has not been reproduced since.

  **The source is refused rather than trusted** when no host is configured, the
  node is unreachable, the reading is older than 20 s, or the current is outside
  0–60 A; the water delta then decides. This is not defensive programming for
  its own sake. Hours before the feature was written, the same outdoor channel
  reported a steady 3.9 A while the compressor was off — its CT's positive input
  was disconnected, and the floating input produced a plausible, load-independent
  number. The house meter caught it: 874 W claimed against a 778 W whole house,
  and 3.75 A on a line-to-line load whose other line carried 1.40 A. Both
  impossible. A firmware that had simply believed its input would have reported
  the compressor permanently running.

  New diagnostic entity **`Compressor source`** publishes `power` or `delta`.
  The two differ by tens of seconds on every edge, so history recorded under one
  is not directly comparable with history recorded under the other, and a silent
  fallback would hide that.

  Configuration follows the existing pattern — host, channel and both thresholds
  in NVS from the Config tab, empty host disabling the source. That is the
  shipped default, so a device never told about a PowerMeter behaves exactly as
  it did before. **1.12.0 reached the device with the endpoint but no Config tab
  fields**, making the feature settable only over HTTP; 1.12.1 adds them, and the
  version moved rather than reusing 1.12.0 for a different binary.

  Ground truth throughout was the indoor unit's **compressor icon** (operation
  manual item 15, *"indicates that the compressor in the outdoor unit is
  active"*), called out by the owner and captured by `tools/mark_compressor.ps1`.
  Worth noting for future work: the call-outs themselves lagged the current step
  by around 34 s, so human observation is fine for labelling states and useless
  for timing edges.

- v1.19 — **Specification and history split into two files (firmware 1.11.0).**
  `FSD_AlthermaInterface.md` becomes the clean current specification — present
  tense, no history — and this file takes the development record, all 32
  entries moved across unchanged. The changelog had reached 671 of the FSD's
  884 lines: **76 % of the document, and §1 did not start until line 684**, so
  anyone opening it to find out what the firmware must do read three quarters
  of a history first. The spec is now 213 lines.
  The release contract in `CLAUDE.md` grows a fourth step and a rule that
  matters more than the file split itself: when a change makes a sentence in
  the spec wrong, rewrite the sentence rather than appending a qualifier
  explaining what it used to say. Otherwise the clean file drifts back into
  being a changelog with headings.

- v1.18 — **A numeric twin for the compressor sensor (firmware 1.11.0), §6,
  §7.** `binary_sensor.espaltherma_compressor` gives on/off history but no
  long-term statistics, and the figure actually worth trending on a heat pump
  is the **duty cycle** — which is exactly the hourly *mean* of the same state
  expressed as 0 or 1. So the state is now published twice: `Compressor` as
  before, and `Compressor numeric` as `sensor.espaltherma_compressor_numeric`
  with `state_class: measurement`.

  Two entities for one fact, deliberately. Deriving the numeric form in Home
  Assistant would need a template sensor written once per install; two lines
  of firmware serve every install. Both come from a single evaluation in
  `alt_derived_update()` — `set_compressor()` writes both forms from one
  decision, so they cannot disagree, and a device reporting `ON` on one entity
  and `0` on the other is a failure mode that simply cannot arise.

  No device class and **no unit**: 0/1 is a flag rendered as a number, not a
  measured quantity in any unit, and inventing a unit to satisfy the usual
  measurement test would be worse than special-casing it. `state_class` is
  therefore added explicitly for this one. In the `ATTR` payload it is the only
  **unquoted** value the firmware emits — a quoted `"1"` would be a string that
  happens to parse, which is not the point of it.

  **Also corrected here:** the FSD header still read `**Firmware:** 1.10.0`
  after v1.17 shipped as 1.10.1 — the changelog entry was updated and the
  header was not. Exactly the kind of split v1.14 and v1.15 were written to
  eliminate, this time in the document rather than the build.

- v1.17 — **Burst sampling: a bounded window at link speed (firmware 1.10.1),
  §6, §11.** The compressor sensor of v1.16 exposed a limit that was already
  there and had never mattered: the fastest selectable poll interval is 30 s,
  while this machine changes state in seconds. A compressor start and stop was
  watched happening entirely between two 60 s polls. A rule can only be as good
  as the sampling that checks it, and a transition that is never sampled cannot
  be argued about.

  `POST /api/burst {"seconds":"N"}` (1–300; the body parser takes quoted values
  only, as everywhere else in this API) makes the poll loop run as fast as the
  X10A link allows for that window, recording each cycle into a ring buffer of
  320 samples that `GET /api/burst` returns as CSV — the same shape as the
  files in `captures/`, so it goes straight into the same analysis. Each sample
  carries its own millisecond timestamp: the rate is whatever a cycle costs and
  is **recorded rather than assumed**.

  Deliberately a window and not a poll-interval option. At one sample a second
  a permanent setting would publish to MQTT every second and mirror a log line
  per registry, flooding the broker for the sake of an occasional experiment —
  so during a burst the per-label logging and the MQTT publish are both
  suppressed, and the cycle after the window closes carries the fresh values. A
  burst expires on its own, so the device cannot be left in this state by
  forgetting about it.

  **1.10.0 shipped to the device with the window unable to open, and 1.10.1
  fixes it.** Arming a burst set a deadline but did not disturb the poll loop,
  which was asleep in a `vTaskDelay` for the rest of its interval — so a window
  armed just after a cycle recorded nothing until the next one, and at the
  slowest interval (480 s) it would expire before the loop ever woke. The wait
  is now `ulTaskNotifyTake`, and `alt_burst_start()` notifies the poll task, so
  sampling begins within a cycle of the request. Caught in the first live test
  rather than by reading the code: `samples=0` on a window that was plainly
  open.

  **Measured rate: about 240 ms per cycle, four samples a second** — five
  registries over a 9600 baud link is quicker than assumed, so "1 Hz" in the
  original design note understated it. Nothing depends on the figure, since
  every sample is timestamped, but it does mean the 320-sample buffer holds
  about **78 s**, not the full 300 s a window may ask for. Beyond that,
  sampling continues and storing stops, with `overflow=yes` in the response
  header saying so rather than quietly dropping data.

  **What prompted it, and what it immediately answered.** On a setpoint drop at
  23:12 the three 60 s samples around it read dT 5.23 → 1.78 → 0.47 K with the
  pump running throughout, so the delta test made the call rather than the pump
  gate. But 1.78 K sits inside the hysteresis band, so `Compressor` held `ON`
  for one extra cycle, and 60 s resolution could not say whether the band was
  too wide or an inverter was genuinely still ramping down.

  A burst across a second, owner-announced stop settled it. 246 samples, 240 ms
  apart:

  | t (s) | dT | refrigerant | |
  |---|---|---|---|
  | 0–27 | 5.78–5.98, flat | 30.4, flat | running |
  | ~28 | 5.58, falling | 30.5 | **the stop** |
  | 39.5 | 3.0 | 30.5 | still `ON` |
  | 45 | 2.0 | 30.3 | still `ON` |
  | 52.1 | 1.12 | 30.3 | **reports `OFF`** |
  | 56+ | 1.0 | 30.1, falling at last | |

  **The `OFF` edge lags the physical stop by about 24 s, and the lag is thermal
  rather than a threshold artifact.** The delta does not step when the
  compressor stops; it decays smoothly at roughly 0.2 K/s as the warm water in
  the exchanger flushes through. Nothing available moves faster — the
  refrigerant liquid side held 30.4 ± 0.2 K for the entire 24 s and only began
  to fall at t ≈ 56, another 30 s later, which vindicates keeping it out of the
  rule by a wide margin.

  It also reinterprets the 23:12 sample: at 1.78 K the compressor had already
  been stopped for some 18 s. The hysteresis was not holding on too long, it was
  describing a machine that had already stopped.

  **The §6 thresholds are therefore kept, now for a measured reason rather than
  caution.** Raising the off threshold to 2.5 K would cut the reported lag from
  24 s to about 13 s while consuming most of the margin to the lowest genuine
  running delta on record (3.0 K). At a 30–60 s poll interval both figures sit
  below the sampling resolution, so the lag is invisible where it matters and
  the false-`OFF` risk would be real. A rate-of-change test could catch a stop
  sooner, but only inside a burst — at normal cadence there is one sample per
  interval and no derivative to take.

- v1.16 — **Derive compressor state and publish it (firmware 1.9.0), §6, §7.**
  The machine reports no compressor field — five registries answer, and `0x53`
  has never shown a non-zero byte outside offsets 0, 3 and 5 — so the one thing
  a heat pump owner most wants to know was the one thing missing from Home
  Assistant. It turns out to be sitting in plain sight in the water
  temperatures.

  With the circulation pump running, `outlet − inlet` is sharply **bimodal**
  across every capture in `captures/`: **0.1–0.9 K** with the compressor idle
  against **3–7 K** with it running, and only 8 of 426 pump-on samples land
  anywhere between 1 and 3 K — all of them mid-transition. The refrigerant
  liquid side confirms it independently (5–6 K below inlet when idle, level
  with it when running) and was deliberately **left out of the rule**: it lags
  minutes behind a stop and spans −4.6 to +1.7 K during genuine operation, so
  ANDing it in would only add false negatives.

  **Confirmed against an observed start.** While this was being written the
  owner reported the compressor had just cut in; the `0x53` reply at that
  moment was byte-identical to the idle-with-pump frame, CRC valid, while the
  water delta stood at 4.77 K and the refrigerant liquid side climbed 5.7 K in
  90 s. That converts "no compressor bit" from an argument out of absence into
  a direct test — see `docs/REGISTER_0x53.md`.

  New `main/derived.c` evaluates it once per poll cycle, between reading and
  publishing, with 2.0 K on / 1.2 K off hysteresis and the pump as a gate — no
  flow means the delta measures nothing, and a stagnant circuit has shown 9.9 K
  while the machine wound down. It appears as `binary_sensor` `Compressor`
  (device class `running`) in Home Assistant discovery, as `"Compressor"` in
  the `ATTR` payload, and as a `calc` row in `/api/values` and the Daikin Data
  tab. While it cannot be determined it is omitted rather than guessed.

  Inputs are located by **registry and offset, not by label**, with the label
  checked once as a guard so that a different definition file disables the
  sensor loudly instead of quietly reinterpreting `0x54` offset 2 as an indoor
  heat exchanger. Two limits stated in §6 and not papered over: a
  space-heating backup heater would read as `ON`, and a 60 s poll can miss a
  short cycle outright — one such cycle was watched starting and stopping
  between two polls while this was being written.

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
