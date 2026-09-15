#pragma once

// Compressor state from the outdoor unit's supply current, read over HTTP from
// a PowerMeter node.
//
// Why this exists: the water-delta rule in derived.c is the only compressor
// signal X10A can give, and its timing is poor. Measured against the current
// step on 2026-09-15, it declared a start 39 s late; the OFF edge is worse,
// because the delta decays thermally at about 0.2 K/s and no faster polling
// would fix it. The outdoor current steps within a second.
//
// It is a SECOND source, not a replacement. derived.c still evaluates the
// water delta every cycle, and falls back to it whenever this one is absent,
// stale or implausible - see alt_compressor_power_state(). Two mechanisms that
// fail differently are worth more than one that fails silently.
//
// Disabled unless a PowerMeter host is configured, so a device that knows
// nothing about one behaves exactly as it did before.

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALT_CP_UNKNOWN = -1,   // no host configured, unreachable, stale or implausible
    ALT_CP_OFF     = 0,
    ALT_CP_ON      = 1,
} alt_cp_state_t;

// The poll loop registers itself here so that a change of compressor state
// can wake it. Without this the state would be known within 5 s but not
// PUBLISHED until the next 30 s cycle, so Home Assistant would still record
// the edge up to 30 s late and most of the benefit would be thrown away.
void alt_compressor_power_register_waiter(TaskHandle_t task);

// Starts the polling task. Safe to call with no host configured - it starts
// nothing and reports UNKNOWN forever.
void alt_compressor_power_start(void);

// Latest state. UNKNOWN whenever the reading cannot be trusted, which is the
// caller's cue to fall back.
alt_cp_state_t alt_compressor_power_state(void);

// Last good current in amps, and its age in seconds. Age is the honest part:
// a reading that stopped updating is worse than no reading, because it looks
// like data.
bool alt_compressor_power_reading(float *amps, int *age_s);

#ifdef __cplusplus
}
#endif
