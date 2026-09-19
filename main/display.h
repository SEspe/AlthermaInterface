#pragma once

// Onboard SSD1306 OLED, 128x64 over I2C.
//
// Optional by design. alt_display_init() probes the panel's address once; if
// nothing answers, every later call is a no-op and the firmware runs exactly as
// it did before. One binary therefore serves both a Lolin board with the
// display and a plain WROOM devkit without it, with nothing to configure and
// nothing to get wrong in the Config tab.
//
// The panel is a status readout, never an input. It shows what the unit already
// publishes - address, compressor state, water and tank temperatures - so
// someone standing at the heat pump can read it without a phone. Nothing here
// influences what is polled or published.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the I2C bus and probes for the panel. Safe to call when no display
// is fitted. Returns true if one was found.
//
// Call before the poll loop starts, so the first cycle can already draw.
bool alt_display_init(void);

// True once a panel has answered. For the boot log and diagnostics.
bool alt_display_present(void);

// Redraws the status page from the converter's current readings and the derived
// compressor state. Call once per poll cycle, AFTER alt_derived_update(), so
// the screen and the published values come from one evaluation.
//
// Silently does nothing when no panel is fitted.
void alt_display_update(void);

// Replaces the screen with a single centred message - used for the boot banner,
// before any reading exists. Ignored when no panel is fitted.
void alt_display_banner(const char *line1, const char *line2);

#ifdef __cplusplus
}
#endif
