#pragma once

// Heat pump model selection. Upstream ESPAltherma does this by uncommenting one
// line in src/setup.h; here it is one file, so the choice is visible and
// greppable. Still compile-time (see docs/PORTING.md, open questions).
//
// Unit: Daikin Altherma LT split hydrobox EKHBH/EKHBX 008BA (~2009-2011).
//
// The BA generation speaks the OLDER "protocol S", not protocol I. Upstream
// documents exactly this family in doc/Daikin S protocol.md ("It seems to be the
// case for DAIKIN EKHBH016BA6WN year 2009" - issue #46). Confirmed on this unit:
// a sweep of both dialects got replies only on the protocol-S registries.
//
// EKHBH = heating only, EKHBX = reversible (heating + cooling); same PCB and
// same registry map, so one definition covers both.
//
// ROTEX variant, not the plain one. Both speak protocol S and poll the same
// registries; they disagree about what the bytes in 0x54 MEAN. Decided from a
// real reply rather than the model number, which is why the file changed:
//
//   0x54 -> 54 fc 18 | 28 1f | b8 1f | b8 1f | 34 25 | 01 00 | 20 00 ...
//
//   plain PROTOCOL_S.h        ROTEX (this file)
//   ------------------------  --------------------------
//   Indoor suction air 24.98  Refrig. liquid side  24.98
//   Indoor heat exch.  31.16  Inlet water          31.16
//   Outdoor air        31.72  Outlet water         31.72
//   Outdoor heat exch. 31.72  D (unknown)          31.72
//   Discharge pipe     74.41  DHW tank             37.20
//
// The plain mapping reports a 74 C discharge pipe while 0x53 says the
// compressor is off, gives an indoor-air reading for a hydrobox that has no
// such sensor, and lands two unrelated sensors on the identical value. The
// ROTEX mapping yields inlet/outlet water 0.56 K apart and a 37 C tank. It also
// matches the hardware: this unit has the 8-pin ROTEX-style X10A, and it answers
// 0x50 with 0x15 0xEA ("not understood"), exactly as this file's header notes.
// def/EKHBH008BA.h is our own file, derived from PROTOCOL_S_ROTEX.h with the
// same 0x53/0x54/0x55/0x56 mapping, plus probes on 0x5A - a registry this unit
// answers that upstream does not document. It also renames the "????" label,
// which would otherwise produce an empty Home Assistant entity key.
// def/PROTOCOL_S_ROTEX.h and def/PROTOCOL_S.h are kept unmodified for reference.
#include "def/EKHBH008BA.h"

#ifndef PROTOCOL
#define PROTOCOL 'I'
#endif
