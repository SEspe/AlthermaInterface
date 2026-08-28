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

## Hypothesis: raw ADC channels

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

## How to settle it

Correlate against a real heating cycle. `main/def/EKHBH008BA.h` already polls
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
query it directly: protocol S, `0x02 0x5A 0xA4`.
