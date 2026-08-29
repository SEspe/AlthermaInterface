# Registry 0x5A — an undocumented protocol-S register

Found on a **Daikin Altherma LT split hydrobox EKHBH/EKHBX 008BA** (BA
generation, ~2009–2011, R410A, 8-pin ROTEX-style X10A), by scanning all 256
registry IDs on protocol S.

For the boolean register `0x53` — what is confirmed there, and the unfinished
search for the 3-way valve — see [`REGISTER_0x53.md`](REGISTER_0x53.md).

`0x5A` appears in **no upstream ESPAltherma definition file**, in **no upstream
documentation**, and in nothing found on GitHub or the wider web. Upstream's 40
definition files use only `0x50`, `0x53`, `0x54`, `0x55` and `0x56` between
them, and its own `doc/list registries.txt` contains no `0x5x` entries at all.

## Scan result

All 256 IDs, protocol S, one query each:

| Outcome | Count |
|---|---|
| replied, CRC valid | **5** — `0x53`, `0x54`, `0x55`, `0x56`, `0x5A` |
| `0x15 0xEA` "not implemented" | 243 |
| replied with bad CRC | 0 |
| silent | 8 |

The 243 explicit refusals matter as much as the hits: this machine states plainly
what it does not implement, so `0x5A` is not an artefact of a confused parser.
Note `0x50` is among the refusals — the ROTEX definition's "0x50 not supported"
note is correct for this unit.

## Shape

18 bytes, same as `0x53`/`0x54`/`0x55`: registry ID, 16 payload bytes, CRC.

```
5a 00 00 60 03 0c 02 43 02 ff 01 05 02 08 00 fc 03 e1
   |___| |___| |___| |___| |___| |___| |___| |___|
    o0    o2    o4    o6    o8   o10   o12   o14
```

Read as little-endian unsigned 16-bit pairs, matching how the rest of this
machine's registers are laid out.

## Observed behaviour

Ten samples, 45 s apart, heat pump idle in Heating with water at ~30 °C:

| Offset | Raw range | Notes |
|---|---|---|
| 0 | 0 | constant |
| 2 | 862–864 | drifts ±2 |
| 4 | 515–524 | drifts ±9 |
| 6 | 575–579 | drifts ±4 |
| 8 | 511–515 | drifts ±4 |
| 10 | 508–520 | drifts ±12 |
| 12 | 6–12 | near zero, drifts |
| 14 | 1020 | constant |

Six channels drift continuously, so this is live data rather than configuration
or field settings.

## CONFIRMED: raw sensor channels behind 0x54

Tested against a real heating cycle on 2026-08-28, 15 samples one minute apart,
circulation pump running, with the compressor cutting in and out so outlet water
swung about 5 K while inlet barely moved.

Correlation of each probe against the `0x54` temperatures:

| probe | inlet | outlet | DHW | refrigerant |
|---|---|---|---|---|
| `5A@2` | 0.255 | 0.380 | −0.057 | 0.225 |
| **`5A@4`** | **−0.928** | −0.277 | −0.084 | −0.009 |
| **`5A@6`** | 0.135 | −0.790 | −0.339 | **−0.992** |
| **`5A@8`** | −0.126 | **−0.958** | −0.256 | −0.835 |
| **`5A@10`** | −0.206 | **−0.994** | −0.244 | −0.764 |
| `5A@12` | 0.156 | 0.120 | 0.212 | 0.275 |

Four channels lock onto **one specific** temperature each, all with **negative**
correlation near −1:

- `5A@4` → inlet water
- `5A@6` → refrigerant liquid side
- `5A@8`, `5A@10` → outlet water

The specificity is what makes this conclusive rather than coincidental. Outlet
and refrigerant are themselves correlated (r = 0.715), so a channel that merely
responded to "the machine got warm" would show up against both. Instead `5A@4`
follows inlet at −0.93 while showing nothing against refrigerant (−0.009), and
`5A@6` follows refrigerant at −0.99 while only weakly following outlet.

Negative correlation is the NTC thermistor signature: resistance falls as
temperature rises, so the ADC count drops. Combined with the 0 and 1020 (10-bit
full scale) bracketing channels, `0x5A` is an **ADC readout of the same sensors
`0x54` reports after conversion**.

So it is a real find, but **not new physical quantities** — it is the
unconverted form of temperatures already available. Its value is diagnostic: a
sensor drifting toward the rails, or an open circuit pinned at full scale, shows
here before conversion hides it.

Still unexplained: `5A@2` (~86.3, very steady) and `5A@12` (~0.9) correlate with
nothing measured. `5A@8` and `5A@10` both track outlet, so one is likely a
different sensor that happens to move with it — a heat exchanger probe, perhaps.
Fifteen samples across one short cycle cannot separate them; a DHW cycle, which
moves the tank temperature independently of the space-heating loop, probably
would.

**Method note:** the first pass at this analysis read the sample columns with an
off-by-one index (the `|` separator is its own field), which attributed each
correlation to the wrong probe and produced a confident but wrong reading of
which channels moved. The table above is from corrected indices. Worth
remembering that eyeballing a wide table is exactly how that error survived
until the numbers were computed.

## The original hypothesis, kept for the reasoning

This was written before the heating cycle, from a single idle sample, and the
prediction it made is what the correlation above went on to confirm.

1. **1020 is essentially 10-bit full scale** (1023).
2. **Offset 0 is pinned at exactly 0.**
3. **Two channels sit at 511–515**, half scale.

A block bracketed by zero and full scale, with mid-scale channels between, looks
like analog sensor inputs alongside calibration references. The ±0.5 % wander on
every live channel reads as analog noise, not as a quantised engineering value
stepping in fixed increments.

If correct, these are the thermistor inputs *before* conversion — the raw form of
sensors whose scaled values already appear in `0x54`. Useful for diagnostics (a
drifting or open-circuit sensor is visible before conversion hides it), but
probably not new physical quantities.

The alternative reading, unsigned/10, gives 0, 86.2, 51.9, 57.9, 51.2, 51.3,
0.9, 102.0 °C. Those are plausible-looking numbers but do not correspond to
anything else this machine reports, and 102.0 never moving argues against it
being a temperature.

## Channel map, after the DHW cycle

The space-heating run could not identify a tank channel, because the tank barely
moved (36.7–37.2 °C). An overnight capture across a **DHW cycle** — 471 samples
a minute apart, tank driven 35.8 → 46.3 °C while outlet water spiked to 55 °C —
supplied the independent movement that was missing.

| Offset | Channel | best match | r |
|---|---|---|---|
| 0 | zero reference | constant 0 | — |
| **2** | **DHW tank** | tank | **−0.992** |
| **4** | **inlet water** | inlet | **−0.999** |
| **6** | **refrigerant liquid side** | refrigerant | **−1.000** |
| **8** | **water circuit, buffered** | outlet while flowing; decouples when flow stops | see below |
| **10** | **outlet water** | outlet | **−0.999** |
| 12 | unidentified | nothing, ≤0.03 | — |
| 14 | full-scale reference | constant 1020 | — |

**`5A@2` is the decisive result.** It sat flat at ~863 counts through hours of
space heating, correlating with nothing, and moved only when the tank did. That
selectivity is what identifies it: the other channels all rose together during
the DHW cycle, because the whole machine warms at once (tank vs outlet r =
0.583), so a strong correlation alone would prove little.

The earlier identifications tightened as well, which is what real sensor channels
should do once exercised across 25 K instead of 5 K: `5A@4` went from −0.928 to
−0.999, `5A@6` to a flat −1.000.

**Since resolved for `5A@8`** — see the next section; the separating condition
turned out to be a pump stop, not a temperature. `5A@12` remains inert — 0.007
against the tank across 257 samples — so whatever it is, it is not a temperature.

### Scaling

These are **ADC counts**, not a physical quantity, and the definition file reads
them with conversion 151 (unsigned 16-bit, unscaled). An earlier reading divided
by 10, which made the first idle sample look like plausible temperatures
(86.4, 51.3, 57.2 …). That was a coincidence of range: 1020 is 10-bit full scale,
and dividing it by 10 happens to land at 102.0.

## Offset 12: an unconnected input

Three hypotheses have been tested against the overnight capture and excluded.

**Not a temperature.** Inert across 471 samples covering a 25 K swing, sitting
at 5–15 counts. On this machine's curve colder means *higher* counts (555 ↔
25.9 °C, ≈ −10 counts/°C), so any plausible temperature would read in the
hundreds. Nine counts is the bottom rail.

**Not outdoor air.** No overnight drift, when outdoor temperature falls steadily
through a night. With outside air measured at 13.5 °C, the curve implies roughly
680 counts; nothing reads near that. Structurally this is the hydrobox — the
outdoor sensor belongs to the outdoor unit.

**Not power or load.** Split by whether the compressor was running
(refrigerant > 35 °C):

| | n | mean | range |
|---|---|---|---|
| compressor running | 41 | 8.6 | 5–14 |
| compressor idle | 428 | 8.9 | 5–15 |

Identical, and marginally *lower* under load. Power consumption would be near
zero at idle and rise sharply with the compressor. For contrast, `5A@8` moved
530 → 301 across the same split, which is what a channel that *is* measuring
something looks like.

**Best explanation: an unconnected ADC input.** A few counts of noise around a
near-zero value, unmoved by temperature, time of day or machine state, is what a
floating or grounded input reads. Note this is a *reading*, not a status field —
a status field would hold discrete steady values rather than wander ±5 counts.

Most likely it is an input for hardware this unit does not have. The ROTEX
definition carries entries for optional kit (`Burner inhibit from solaris`), and
a solar, second-zone or external-tank sensor input that was never wired would
read exactly like this. A machine with that option fitted would be the test.

## What is still open

- **`5A@8`’s physical location.** Its *behaviour* is now pinned down (next
  section): a water-circuit sensor with a long thermal time constant. Which body
  it sits in is inference. The test that would settle it is a run with the backup
  heater driving the **space-heating** circuit rather than the tank booster — a
  sensor downstream of the element should then read hotter than `5A@10` while
  flow continues. No capture has produced that condition yet.
- **`5A@8` vs the estimate** — the interpolated `~n °C est.` shown in the web UI
  assumes it shares the water-circuit curve. Everything observed is consistent
  with that, and the equilibrium behaviour above supports it, but it remains an
  assumption rather than a measurement.
- **`5A@12`** — still best explained as an unconnected input (section above).

`main/def/EKHBH008BA.h` polls `0x5A` every cycle, so the channels are in every
MQTT payload and in Home Assistant history. Answering the above needs no
firmware change, only the right operating mode in the recorded history.

## RESOLVED: separating `5A@8` from `5A@10`

The prediction above was that a defrost or cooling cycle would be needed. What
actually separated them was neither — it was a **pump stop**, caught on
2026-08-29 during a forced DHW run (`captures/dhw_forced.csv`, 347 decoded
samples, 09:14–09:59).

While anything circulates, the two channels are the same reading for all
practical purposes: mean difference **0.4 counts**, r = 0.996. The moment the
circulation pump stopped, with the tank booster still energised, they came apart.

| time | pump | inlet | outlet | `5A@4` | `5A@8` | `5A@10` |
|---|---|---|---|---|---|---|
| 09:45:45 | ON→OFF | 47.0 | 51.0 | 346 | 307 | 305 |
| 09:46:48 | OFF | 32.1 | 42.0 | 493 | 308 | 391 |
| 09:47:44 | OFF | 30.7 | 32.3 | 503 | **315** | **492** |
| 09:48:48 | OFF | 31.5 | 31.5 | 496 | 392 | 502 |
| 09:50:47 | OFF | 33.5 | 33.3 | 471 | 478 | 479 |

Inlet, outlet and their ADC channels all collapse within 90 s of the flow
stopping. `5A@8` does not move at all across those 90 s, then decays over roughly
three minutes until it meets the others. At the peak the two are **177 counts
apart** and r has fallen to **0.635**.

### The control

Pump, DHW priority and heater state vary independently in this capture, so each
can be tested on its own. Mean `5A@8` − `5A@10`, in counts:

| pump | DHW prio | ext heat | n | mean | sd | r |
|---|---|---|---|---|---|---|
| OFF | OFF | OFF | 114 | −3.1 | 7.2 | 0.872 |
| OFF | OFF | **ON** | 38 | **−82.9** | 64.8 | 0.635 |
| ON | ON | OFF | 169 | +2.8 | 6.5 | 0.993 |
| ON | ON | **ON** | 26 | **+0.2** | 1.9 | 0.760 |

The heater is **not** the cause: with the pump running, switching it on moves the
difference by −2.6 counts. DHW priority is not the cause either (+5.9). Only
losing flow does it — and then only while there is still heat in the machine to
hold, which is why the pump-off/heater-off cell is unremarkable at −3.1.

This control matters. The first reading of this data blamed the heater, because
the heater-on rows do show the divergence — they are also, in this capture, the
rows where the pump had stopped. Splitting the two apart reverses the conclusion.

### What it means

`5A@8` is a **water-circuit sensor**: it settles to the same temperature as inlet
and outlet once the machine equilibrates, and it is nowhere near the tank, which
was 20 K hotter and still climbing throughout the window. But it has a far longer
thermal time constant than anything sitting in flowing water, so it is reading a
body that *holds* water — a vessel or an exchanger mass, not a pipe.

On this hydrobox the backup heater vessel fits that description, and it would
also explain why the channel is indistinguishable from leaving water whenever the
pump runs: the water passes straight through it. **That location is an inference
from thermal behaviour; only the lag is measured.**


## Reproducing

`POST /api/scan` on the device (`GET /api/scan` for progress and results), or
query it directly: protocol S, `0x02 0x5A 0xA3`.
