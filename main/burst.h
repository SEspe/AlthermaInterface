#pragma once

// Burst sampling: a bounded window where the poll loop runs as fast as the
// X10A link allows instead of once per configured interval, recording each
// cycle into a ring buffer the web UI can read back.
//
// Why this exists: the normal poll interval is 30 s at its fastest, and the
// machine changes state in seconds. A compressor start and stop was watched
// happening entirely between two 60 s polls, which is invisible to the
// firmware and to anything downstream of it. Deriving compressor state from
// the water delta (derived.h) made that resolution limit matter: the rule is
// only as good as the sampling, and it could not be checked against a
// transition that was never sampled.
//
// It is deliberately a WINDOW and not a setting. A permanent 1 Hz interval
// would publish to MQTT and mirror a log line every second, and would keep the
// service port busy forever for the sake of an occasional experiment. A burst
// expires on its own, so the device cannot be left in this state by forgetting
// about it.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bounds. The window may be asked for up to 300 s, but the buffer is the real
// limit: measured on this link a cycle costs about 240 ms, so 320 samples is
// roughly 78 s of recording, after which sampling continues and storing stops
// with the overflow flag set. 60 s covers a start or a stop with margin - a
// stop took 24 s from the compressor cutting out to the water delta settling.
#define ALT_BURST_MAX_SECONDS 300
#define ALT_BURST_MAX_SAMPLES 320

// The poll loop registers itself here so that arming a burst can wake it.
// Without this a burst armed just after a cycle would sit idle for the rest of
// the interval - up to 8 minutes on the slowest setting, which outlasts the
// longest window and would record nothing at all.
void alt_burst_register_waiter(TaskHandle_t task);

// Arms a burst of `seconds` (1..ALT_BURST_MAX_SECONDS), discarding any samples
// from a previous one. ESP_ERR_INVALID_ARG outside the range,
// ESP_ERR_INVALID_STATE if one is already running.
esp_err_t alt_burst_start(int seconds);

// True while the window is open. The poll loop reads this to decide its pace,
// whether to publish, and whether to log per-label lines - at 1 Hz the MQTT log
// mirror would otherwise send tens of lines a second.
bool alt_burst_active(void);

// Appends one sample from the converter's current values. Called by the poll
// loop after the registries are decoded and derived values updated.
void alt_burst_record(void);

// Samples held, and whether any were dropped because the buffer filled.
size_t alt_burst_count(void);
bool   alt_burst_overflowed(void);

// Writes sample `i` as one CSV row (no trailing newline). Returns false when i
// is past the end. Column order matches alt_burst_csv_header().
bool alt_burst_row(size_t i, char *out, size_t out_len);

const char *alt_burst_csv_header(void);

#ifdef __cplusplus
}
#endif
