#pragma once

// Derived values: machine state that X10A does not report directly but that
// the reported values determine.
//
// This unit's PCB exposes no compressor field at all. The 256-ID scan found
// five registries that answer (0x53, 0x54, 0x55, 0x56, 0x5A); 0x53 is the
// boolean register and only its offsets 0, 3 and 5 have ever been non-zero, so
// there is no unmapped compressor bit hiding in it. The plain PROTOCOL_S.h map
// does carry "INV Comp. Frequency", but that mapping was rejected against real
// bytes (main/model_config.h) - those fields belong to an outdoor-unit PCB this
// service port does not reach.
//
// So it is inferred. See FSD section 6 and alt_derived_update() for the rule
// and the evidence behind it.

#ifdef __cplusplus
extern "C" {
#endif

// The label these values are published under, on MQTT and in Home Assistant
// discovery. One definition so the two cannot drift apart.
#define ALT_DERIVED_COMPRESSOR_LABEL "Compressor"

// Recomputes every derived value from the converter's current readings. Call
// once per poll cycle, after the registries have been read and before
// publishing, so the web UI and MQTT report the same evaluation rather than two
// made moments apart.
void alt_derived_update(void);

// "ON", "OFF", or "" when it cannot be determined - no reading yet, or the
// active definition file does not provide the inputs. Publishers skip empty
// values, exactly as they do for a label never read.
const char *alt_derived_compressor(void);

#ifdef __cplusplus
}
#endif
