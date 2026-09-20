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

// Why the last poll failed. "It is not working" has several distinct causes
// here and they need completely different fixes - a wrong host, a node that is
// down, and a mistyped channel label all look identical from the compressor
// sensor, which simply falls back to the water delta either way.
typedef enum {
    ALT_CP_ERR_NONE = 0,     // last poll succeeded
    ALT_CP_ERR_DISABLED,     // no host configured - not an error
    ALT_CP_ERR_CONNECT,      // no route, refused, or timed out
    ALT_CP_ERR_HTTP,         // answered, but not with 200
    ALT_CP_ERR_EMPTY,        // 200 with no body
    ALT_CP_ERR_CHANNEL,      // body fine, channel label not in it
    ALT_CP_ERR_IMPLAUSIBLE,  // parsed, but outside 0-60 A
} alt_cp_err_t;

// Short human-readable name for a failure reason.
const char *alt_compressor_power_err_name(alt_cp_err_t e);

typedef struct {
    bool           enabled;      // a host is configured
    alt_cp_state_t state;        // as alt_compressor_power_state()
    float          amps;         // last good reading
    int            age_s;        // its age; -1 = never read
    uint32_t       polls;        // attempts since boot
    uint32_t       ok;           // of which succeeded
    uint32_t       fail;         // of which did not
    alt_cp_err_t   last_err;
    int            http_status;  // status of the last HTTP reply, 0 if none
} alt_cp_stats_t;

// Snapshot for the Debug tab. Safe to call before the task starts.
void alt_compressor_power_stats(alt_cp_stats_t *out);

#ifdef __cplusplus
}
#endif
