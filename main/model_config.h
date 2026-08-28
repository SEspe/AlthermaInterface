#pragma once

// Heat pump model selection. Upstream ESPAltherma does this by uncommenting one
// line in src/setup.h; here it is one file, so the choice is visible and
// greppable. Still compile-time (see docs/PORTING.md, open questions).
//
// Unit: Daikin Altherma LT split hydrobox EKHBH/EKHBX 008BA (~2009-2011).
//
// The BA generation speaks the OLDER "protocol S", not protocol I. Upstream
// documents exactly this family in doc/Daikin S protocol.md ("It seems to be the
// case for DAIKIN EKHBH016BA6WN year 2009" - issue #46). PROTOCOL_S.h defines
// PROTOCOL 'S', which switches the query framing and the fixed reply lengths.
//
// EKHBH = heating only, EKHBX = reversible (heating + cooling); same PCB and
// same registry map, so one definition covers both.
#include "def/PROTOCOL_S.h"

#ifndef PROTOCOL
#define PROTOCOL 'I'
#endif
