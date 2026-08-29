# Registry 0x53 — the boolean register, and the 3-way valve

Observed on a **Daikin Altherma LT split hydrobox EKHBH/EKHBX 008BA** (BA
generation, ~2009–2011, R410A, 8-pin ROTEX-style X10A), protocol S.

Unlike [`0x5A`](REGISTER_0x5A.md), `0x53` is *not* undocumented — upstream
ESPAltherma's `PROTOCOL_S_ROTEX.h` maps four of its bits. This document records
what has been **confirmed against observed hardware** on this specific machine,
which of upstream's labels remain unverified guesses, and — the question that
motivated it — how to tell **domestic hot water from space heating**, which
payload offset 5 answers.

## Shape

18 bytes, the protocol-S default: registry ID, 16 payload bytes, CRC.

```
53 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ab
   |  |  |  |  |  |
   o0 o1 o2 o3 o4 o5 ...
```

Payload offset *N* is `buf[N+1]` — the registry ID occupies `buf[0]`. Getting
this off by one is easy and has produced a confidently wrong identification in
this project before; every claim below was checked against the raw bytes.

Each byte is a boolean: `0x00` or `0x01`. No packed bitfields have been seen —
every observed change moved a whole byte, never an individual bit within one.

## Upstream's mapping

`PROTOCOL_S_ROTEX.h` assigns four offsets and leaves twelve unmapped:

| Offset | Upstream label | Status here |
|---|---|---|
| 0 | `Circulation pump` | **CONFIRMED** — stopping the room thermostat drove it 1 → 0 |
| 3 | `External heater?` | **CONFIRMED** — contactor heard engaging three times |
| 5 | `Priority to domestic water` | **CONFIRMED** — the 3-way valve was *watched moving* as it went 0 → 1 |
| 6 | `Burner inhibit from solaris` | never seen non-zero |
| 1, 2, 4, 7–15 | *unmapped* | never seen non-zero, including mid-DHW |

Three of upstream's four labels are now verified on this machine, each by a
deliberate physical action rather than by correlation.

Note upstream's own question mark on offset 3, and that
`Burner inhibit from solaris` is a ROTEX-ism for equipment this unit does not
have. Twelve unmapped bytes is a lot of room for the valve to hide in.

## CONFIRMED: offset 3 is the electric heater contactor

On **2026-08-29** the machine was watched directly while a DHW cycle was
requested from the user interface with a warm tank. The request was declined,
but the **electric heater contactor was heard engaging three times** — and the
captures show payload offset 3 flipping `0x00` → `0x01` in exactly three
clusters:

| Engagement | Samples | Span |
|---|---|---|
| 1 | 08:59:14, :28, :31, :36 | ≥ 22 s |
| 2 | 09:00:53, :54 | ≤ 15 s |
| 3 | 09:01:10 (×2) | ≤ 15 s |

The two raw replies differ in one byte and nothing else:

```
off:  53 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ab
on:   53 01 00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 aa
                  ^^ buf[4] = payload offset 3           ^^ CRC
```

The CRC corroborates the framing: `~sum` drops by exactly one count
(`0xab` → `0xaa`) when a single payload byte rises by one. Nothing was
misframed or misaligned.

So upstream's question mark can be removed: offset 3 is the electric backup
heater contactor. This definition therefore labels it
`Electric heater contactor`.

### Why this bit is worth publishing to Home Assistant

Resistive backup heat is the expensive mode of operation — several times the
running cost of the compressor for the same delivered heat. A binary sensor on
this bit is effectively a "this period costs much more than usual" indicator,
which is more actionable than most of the temperature channels.

## Two methodological warnings

**A brief contactor pulse produces no measurable water temperature change.**
The first analysis of this event looked for a rise in outlet water temperature,
found none across the pulses, and concluded the "external heater" label was
doubtful. That reasoning was wrong: a 15–30 s resistive pulse is swamped by the
thermal mass of the circulating volume. Absence of a thermal response over such
a span is not evidence that a bit is lying. The correct check was the one the
owner performed — listening to the contactor.

**Polling at 15 s cannot count contactor events.** A pulse shorter than the
interval is invisible, and two pulses close together are indistinguishable from
one long one. These captures are adequate for "is the expensive heater involved
right now" and **not** adequate for runtime totals or duty cycle. Deriving
energy figures from this data would be unsound.

A partial mitigation used here: three captures were running concurrently at
different intervals and unsynchronised phases, so their samples interleave and
give better effective resolution than any one alone. That is what separated
engagement 2 from engagement 3.

## CONFIRMED: offset 5 is DHW, and it tracks the 3-way valve

The motivating question was whether X10A reveals **which way the 3-way valve is
pointing** — heating the house, or heating the DHW tank. **It does**, on
`0x53` payload offset 5.

On **2026-08-29** the test was made deliberately clean. Space heating was
stopped at the room thermostat first, so nothing was running and `0x53` read
**all sixteen payload bytes zero** — a baseline no earlier capture had. The DHW
setpoint was then raised above the tank temperature to force a call.

```
09:21:47  53 00 00 00 00 00 00 ...  0xac   idle, pump off, all zero
09:22:17  53 01 00 00 00 00 01 ...  0xaa   pump ON + DHW priority ON
             ^^ offset 0            ^^ offset 5
```

The owner **watched the 3-way valve physically divert** at that moment. That is
what makes this a position result and not merely a mode flag: the bit and the
valve moved together, observed directly.

The hydraulics confirm heat actually went to the tank rather than the house:

| | 09:22:38 | 09:22:45 | 09:23:13 |
|---|---|---|---|
| inlet water | 30.41 | 29.33 | 31.81 |
| outlet water | 30.88 | **34.44** | **37.78** |
| refrigerant | 25.98 | 27.97 | 29.41 |

Outlet rose 6.9 K within a minute of the flag, opening a ~6 K delta across the
plate with the compressor running.

Note the DHW **tank** reading keeps drifting *down* for the first minutes of the
cycle (45.39 → 44.67). The tank sensor lags the coil considerably, so tank
temperature is a poor and slow indicator of "DHW is running". Offset 5 is
immediate.

Throughout the whole cycle `0x53` took exactly **two** values — the all-zero idle
state and the pump+DHW state above. All twelve unmapped bytes stayed zero even
mid-DHW, so there is no separate valve byte: offset 5 carries it.

### The four states of a complete DHW cycle

The forced cycle of 2026-08-29 produced exactly four distinct `0x53` values and
no others, over 264 samples at 5 s:

| Payload | Count | off0 pump | off3 heater | off5 DHW | CRC |
|---|---|---|---|---|---|
| all zero | 63 | – | – | – | `0xac` |
| `01 … 01` | 169 | ● | – | ● | `0xaa` |
| `01 … 01 … 01` | 26 | ● | ● | ● | `0xa9` |
| `… 01 …` | 6 | – | ● | – | `0xab` |

The CRC independently validates every one: `~sum` falls by exactly one count per
bit set — `0xac`, `0xab`, `0xaa`, `0xa9` for zero, one, two and three bits. No
state was misframed.

The cycle ran in this order, from 44.8 °C to a 53 °C setpoint in **27m27s**:

| Phase | State | Duration | Tank |
|---|---|---|---|
| heat pump alone | pump + DHW | 09:22:17 → 09:42:14 (19m57s) | 44.81 → 45.11 |
| heat pump + booster | pump + DHW + heater | 09:42:14 → 09:45:17 (3m03s) | 45.11 → 48.56 |
| booster alone | heater | 09:45:17 → 09:49:44 (4m27s) | 48.56 → 52.98 |
| finished | all zero | 09:49:44 | 52.98 |

The heater cut out at 52.98 °C, essentially exactly on setpoint. Electric heat was
involved for 7.5 of the 27.5 minutes.

**Do not read the tank column as an efficiency comparison.** The heat pump spent
its first twenty minutes lifting the water circuit from 30 °C to 51 °C rather
than the tank, and the tank sensor lags the coil badly, so temperature gain
cannot be attributed to a phase. The state sequence and the durations are sound;
K-per-minute figures derived from this table would not be.

### What offset 3 actually heats

The final phase is the informative one. With **pump off, DHW flag off and no
circulation at all**, the tank still rose (48.56 → 49.13 °C) while inlet and
outlet water drifted *downwards* as residual heat dissipated. Heat was therefore
entering the tank directly, not through the plate heat exchanger.

That points to offset 3 being an **immersion element in the DHW tank** — the
booster — rather than a backup heater in the water circuit. It also explains the
2026-08-29 morning observation, where the bit pulsed three times during a
*refused* DHW request with the pump never running.

The label is nonetheless left as the generic `Electric heater contactor`,
because this unit may also have a space-heating backup heater and it is untested
whether that would report on the same bit. Claiming "DHW booster" would assert
more than has been observed. What is established: **this bit can heat the tank
with no circulation.**

### What does NOT indicate DHW

- **`0x55` offset 0 `Operation Mode`** reads `"Heating"` continuously — through
  a full overnight DHW cycle, through this forced cycle, and *even while the
  machine sat completely idle with the pump off*. It is evidently the configured
  season/mode, not a running state, and it cannot tell you what the machine is
  doing. Anything built on it will be wrong.
- **DHW tank temperature rising** works eventually but lags by minutes, and
  falls during the first part of a cycle.
- `0x55` and `0x56` did not change at all across the entire experiment.

### The caveat that was ruled out

Before the test it was noted that most 3-way diverter valves are open-loop — the
controller drives the actuator and assumes the position, with no feedback
switch. Had that been the case here, a bit could reflect only the *commanded*
state. Watching the valve move in step with the flag settles it for practical
purposes; distinguishing true position feedback from a perfectly-tracking
command would need the valve forced out of position while the machine is idle.

## Reproducing

`/api/x10a` exposes the last raw reply per registry, so no firmware change is
needed to capture this — poll it alongside `/api/values` and record both.

Beware that the value lookup in any such script keys on the **label string**,
which is also what drives the Home Assistant `uniq_id` and the value template.
Renaming a label in the definition file therefore silently blanks that column in
a running capture. The scripts used here accept either the old or the new label
for this reason.
