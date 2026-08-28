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
//  in no published documentation. The entries below are PROBES, not knowledge:
//  each 16-bit pair of the payload read as unsigned/10, which is the scaling
//  that made the first sample look physical (0, 86.4, 51.3, 57.2, 51.0, 51.9,
//  0.9, 102.0). Watch which of them move, and against what.

// dataType is not used by the converter at all - it exists purely so Home
// Assistant discovery can attach a device class. Every upstream protocol-S
// definition leaves it -1, which makes every temperature arrive in HA as a
// unitless number with no unit, no device class and no statistics. Setting it
// here is using upstream's own mechanism, not deviating from it.
#define HA_TEMPERATURE 1
#define HA_PRESSURE    2

LabelDef labelDefs[] = {
    // ---- 0x53: booleans (upstream ROTEX mapping) ----
    {0x53, 0, 200, 1, -1, "Circulation pump"},
    {0x53, 3, 200, 1, -1, "External heater?"},
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

    // ---- 0x5A: undocumented, found by scanning. Probes only. ----
    {0x5A, 0,  155, 2, -1, "Probe 5A at 0"},
    {0x5A, 2,  155, 2, -1, "Probe 5A at 2"},
    {0x5A, 4,  155, 2, -1, "Probe 5A at 4"},
    {0x5A, 6,  155, 2, -1, "Probe 5A at 6"},
    {0x5A, 8,  155, 2, -1, "Probe 5A at 8"},
    {0x5A, 10, 155, 2, -1, "Probe 5A at 10"},
    {0x5A, 12, 155, 2, -1, "Probe 5A at 12"},
    {0x5A, 14, 155, 2, -1, "Probe 5A at 14"},
};

// Override protocol
#define PROTOCOL 'S'
