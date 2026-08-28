# Registry 0x5A — an undocumented protocol-S register

Found on a **Daikin Altherma LT split hydrobox EKHBH/EKHBX 008BA** (BA
generation, ~2009–2011, R410A, 8-pin ROTEX-style X10A), by scanning all 256
registry IDs on protocol S.

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

## Original hypothesis (now confirmed)

Not established — recorded so it can be tested or discarded.

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

## How it was settled

Correlated against a real heating cycle - see the confirmed section above. `main/def/EKHBH008BA.h` already polls
`0x5A` every cycle as eight `Probe 5A at N` labels, so both `0x54` temperatures
and `0x5A` channels are in every MQTT payload and in Home Assistant history.

- If offsets 4/6/8/10 move **inversely** to the `0x54` temperatures, they are NTC
  thermistor readings and the ADC hypothesis holds.
- If they track a temperature **proportionally** with a consistent scale factor,
  they are scaled values and the divisor can be derived from the ratio.
- If they do not correlate with anything, they belong to some other subsystem.

A day of history with the compressor actually running should be decisive. At
~30 °C and idle there is nothing to correlate against.

## Reproducing

`POST /api/scan` on the device (`GET /api/scan` for progress and results), or
query it directly: protocol S, `0x02 0x5A 0xA3`.
