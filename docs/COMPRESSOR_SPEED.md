# Compressor speed on the Daikin Altherma EKHBH008BA

Question: can the firmware read the inverter compressor's speed (frequency) from
this heat pump, so that its output and efficiency can be judged?

**Answer: no, not over the X10A serial link on this unit.** The compressor is
inferred from the outdoor unit's supply current instead (FSD section 6).

## Evidence

**1. The protocol-S map has a frequency field, but it is empty on this unit.**
`main/def/PROTOCOL_S.h` lists `INV Comp. Frequency(Hz)` at register `0x53`,
offset 4. A capture at a confirmed compressor start (`REGISTER_0x53.md`) reads
`0x53` as byte-identical to the idle frame, with only offset 0 set (circulation
pump). Offset 4 is zero while the compressor is running. The mapping was
rejected against these bytes, and the field belongs to a different outdoor PCB.

**2. A full register scan has no speed register.** All 256 registry IDs were
queried on protocol S (`REGISTER_0x5A.md`). Five answer with a valid CRC:
`0x53`, `0x54`, `0x55`, `0x56` and `0x5A`. The other 243 return `0x15 0xEA`
("not implemented"). Eight are silent. The protocol-I dialect answers only on
protocol-S registries, so it gives nothing extra.

**3. The unlabelled bytes do not track the compressor.** Across 200 decoded
samples from the cycle logs (86 with the compressor on):

| Field | Behaviour |
|---|---|
| `Unknown 0x56` | constant 48.004 |
| `Unknown 0x54` offset 10 | constant 0.004 |
| `Delta-Tr` (`0x54` offset 12) | constant 33 until about 20 Sep, then 30–42 (curve setpoint, published as "Curve setpoint (C)") |
| `Unknown 0x54` offset 6 | moves with the compressor (mean 35.2 °C on, 31.6 °C off), so it is a temperature, not a speed |

`0x5A` was already tested and excluded as a power or load signal (raw sensor
ADC readings, see `REGISTER_0x5A.md`).

**4. The `0x30` "INV frequency (rps)" entry is for other models.** An external
ESPHome project, `Dennis3321/DaikinAlthermaX10AConnect`
(`daikin-x10a.yaml`), defines register `0x30`, offset 0, as "INV frequency
(rps)". In the local upstream clone, that same entry appears only in
definition files for other Altherma models (LT CA/CB, hybrid and multi-DHW
units), and it is commented out there. No protocol-S definition uses `0x30`,
and this unit's protocol-S scan did not answer on it.

## Conclusion

The compressor frequency is not readable over the X10A serial link on this
unit. Any speed value would have to come from another source.

## Options for judging output and efficiency

- **Outdoor supply power** from the PowerMeter outdoor unit (`.42`). The
  compressor's electrical input. Current tracks inverter load, so it works as a
  rough speed proxy. The current-to-speed relationship is not verified on this
  unit.
- **Heat output**, from water flow rate and the inlet and outlet temperatures
  already logged. Flow is not measured, so a flow meter is needed.
- **COP** = heat output / electrical input, once both are measured per interval.
  Spare capacity also needs the rated maximum at each outdoor temperature from
  the Daikin data sheet.

## Open items

- [ ] Check whether `0x54` offset 6 (the temperature that tracks the compressor)
  matches the discharge line or another body, by comparing it with the outdoor
  supply current over a load change.
- [ ] Measure outdoor supply power and water flow to get a real COP.
- [ ] Confirm the outdoor current-to-load relationship with a known resistive
  load, before using current as a speed proxy.

## Sources

- `main/def/PROTOCOL_S.h`, `main/derived.h`, `docs/REGISTER_0x53.md`,
  `docs/REGISTER_0x5A.md`, `main/def/EKHBH008BA.h`
- Cycle logs in `captures/cycles_*.log`
- Upstream reference: `upstream-ESPAltherma/include/def/ALTHERMA(*).h`
  (`0x30` entries, commented out)
- External: [Dennis3321/DaikinAlthermaX10AConnect](https://github.com/Dennis3321/DaikinAlthermaX10AConnect)
  (`daikin-x10a.yaml`)
