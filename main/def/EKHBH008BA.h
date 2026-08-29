#include "labeldef.h"
//  Definition file for THIS unit: Daikin Altherma LT split hydrobox
//  EKHBH / EKHBX 008BA (BA generation, ~2009-2011, R410A), protocol S.
//
//  Derived from upstream ESPAltherma's def/PROTOCOL_S_ROTEX.h
//  (MIT, (c) 2020 Raomin) - the 0x53/0x54/0x55/0x56 mapping below is theirs,
//  confirmed correct on this machine by comparing both protocol-S mappings
//  against a real reply (see docs/PORTING.md).
//
//  What is new here is 0x5A. A scan of all 256 registry IDs on this unit found
//  it answering with 18 bytes and a valid CRC, while 243 other IDs replied
//  0x15 0xEA ("not implemented"). It appears in no upstream definition file and
//  in no published documentation. Its channels have since been identified by
//  correlation against a real heating and DHW cycle - see the notes below and
//  docs/REGISTER_0x5A.md.

// dataType is not used by the converter at all - it exists purely so Home
// Assistant discovery can attach a device class. Every upstream protocol-S
// definition leaves it -1, which makes every temperature arrive in HA as a
// unitless number with no unit, no device class and no statistics. Setting it
// here is using upstream's own mechanism, not deviating from it.
#define HA_TEMPERATURE 1
#define HA_PRESSURE    2

LabelDef labelDefs[] = {
    // ---- 0x53: booleans (upstream ROTEX mapping) ----
    //
    // Offset 3 is CONFIRMED against observed hardware: the payload byte flips
    // 0x00 -> 0x01 exactly when the electric heater contactor engages, watched
    // at the unit on 2026-08-29. Upstream labels it "External heater?" with a
    // question mark; the question is answered, so the mark is dropped.
    //
    // Note for anyone testing this: a contactor pulse of 15-30 s produces no
    // measurable change in outlet water temperature, because the circulating
    // volume swamps it. Absence of a thermal response is not evidence the bit
    // is lying.
    {0x53, 0, 200, 1, -1, "Circulation pump"},
    {0x53, 3, 200, 1, -1, "Electric heater contactor"},
    {0x53, 5, 200, 1, -1, "Priority to domestic water"},
    {0x53, 6, 200, 1, -1, "Burner inhibit from solaris"},

    // ---- 0x54: temperatures (upstream ROTEX mapping) ----
    {0x54, 0, 153, 2, HA_TEMPERATURE, "Refrig. Temp. liquid side(C)"},
    {0x54, 2, 153, 2, HA_TEMPERATURE, "Inlet water temp.(C)"},
    {0x54, 4, 153, 2, HA_TEMPERATURE, "Outlet Water Temp.(C)"},
    {0x54, 6, 153, 2, HA_TEMPERATURE, "Unknown 0x54 offset 6(C)"},
    {0x54, 8, 153, 2, HA_TEMPERATURE, "DHW tank temp.(C)"},
    {0x54, 10, 103, 2, HA_TEMPERATURE, "Unknown 0x54 offset 10(C)"},
    {0x54, 12, 101, 1, -1, "Delta-Tr(deg)"},
    {0x54, 13, 151, 1, -1, "R-C Setpoint(C)"},

    // ---- 0x55: status (upstream ROTEX mapping) ----
    {0x55, 0, 201, 1, -1, "Operation Mode"},
    {0x55, 1, 204, 1, -1, "Error Code"},
    {0x55, 2, 204, 1, -1, "Thermo Off Error"},
    {0x55, 3, 204, 1, -1, "Warning Code"},
    {0x55, 4, 204, 1, -1, "Caution Code"},

    // Upstream labels this "????", which breaks Home Assistant discovery: the
    // entity key is built by dropping every non-alphanumeric character, so
    // "????" yields an EMPTY key and a unique_id of just "espaltherma_".
    {0x56, 0, 103, 2, HA_TEMPERATURE, "Unknown 0x56"},

    // ---- 0x5A: undocumented upstream; identified here ----
    //
    // Found by scanning all 256 registry IDs, then identified by correlating
    // each channel against the 0x54 temperatures across a space-heating run and
    // a DHW cycle (docs/REGISTER_0x5A.md). It is the RAW ADC readout of the
    // sensors 0x54 reports converted: every channel moves inversely to its
    // temperature, which is the NTC thermistor signature, and the block is
    // bracketed by a zero channel and a full-scale one.
    //
    // Conversion 151 - unsigned 16-bit, unscaled - because these are ADC counts
    // and not a physical quantity. An earlier /10 reading made them look like
    // plausible temperatures, which was a coincidence of range, not meaning.
    // dataType stays -1: no device class, since the unit is "counts".
    //
    // Their value is diagnostic. A sensor drifting toward either rail, or open
    // circuit at full scale, shows here before the conversion in 0x54 hides it.
    {0x5A, 0,  151, 2, -1, "ADC zero reference"},          // constant 0
    {0x5A, 2,  151, 2, -1, "ADC DHW tank"},                // r = -0.992 vs tank
    {0x5A, 4,  151, 2, -1, "ADC inlet water"},             // r = -0.999
    {0x5A, 6,  151, 2, -1, "ADC refrigerant liquid side"}, // r = -1.000
    // Shadows inlet water within a couple of counts and swings with the
    // compressor (530 -> 301 during a DHW cycle), so it is a water-circuit
    // sensor - but it has not been separated from offset 10 (r = 0.977).
    {0x5A, 8,  151, 2, -1, "ADC water circuit (unassigned)"},
    {0x5A, 10, 151, 2, -1, "ADC outlet water"},            // r = -0.999
    // Inert: 5-15 counts regardless of temperature, time of day or whether the
    // compressor runs. Not a temperature, not outdoor air, not power - all three
    // tested and excluded (docs/REGISTER_0x5A.md). Reads like an unconnected
    // input, most likely for optional hardware this unit does not have.
    {0x5A, 12, 151, 2, -1, "ADC unused input"},
    {0x5A, 14, 151, 2, -1, "ADC full-scale reference"},    // constant 1020
};

// Override protocol
#define PROTOCOL 'S'
