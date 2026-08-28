#pragma once

// X10A service-port link: framing, CRC and registry queries for the Daikin
// "protocol I" and "protocol S" dialects. Ported from upstream ESPAltherma's
// include/comm.h (MIT, (c) 2020 Raomin) - see docs/PORTING.md.
//
// This unit (EKHBH/EKHBX 008BA) speaks protocol S; protocol I is carried along
// because it is only a few lines of difference.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Longest reply any protocol produces is 18 bytes; upstream uses 64.
#define ALT_REPLY_MAX 64

// Configures UART1 on the pins in board_config.h (9600 8E1). Call once.
esp_err_t alt_serial_init(void);

// Sends a registry query and reads the reply into buf.
// Returns the number of bytes read on success (CRC verified), or -1 on
// timeout, CRC failure, or an explicit error reply from the heat pump.
// buf must hold at least ALT_REPLY_MAX bytes.
int alt_query_registry(uint8_t reg_id, uint8_t *buf, char protocol);

// Daikin checksum: sum the bytes, then invert. Same for both protocols.
uint8_t alt_crc(const uint8_t *src, size_t len);

// Formats up to `len` bytes as "0x00 0x01 ..." into out (for logging).
void alt_format_buffer(const uint8_t *buf, size_t len, char *out, size_t out_len);

// ---- link diagnostics -------------------------------------------------
// The unit runs off X10A's 5 V inside an enclosure, with no USB attached, so
// the serial log is not available where it matters. Every query records its
// outcome here instead, and the web UI reads it back over WiFi.

#define ALT_DIAG_MAX 24

// Reads back the last outcome for the i-th (protocol, registry) pair queried
// since boot. `status` is a short human-readable result ("ok",
// "timeout: no bytes", ...) and `hex` the raw bytes received, both pointing at
// internal storage. Returns false when i is past the number of pairs seen.
bool alt_diag_at(size_t i, uint8_t *reg_id, char *protocol, const char **status,
                 const char **hex, int *bytes, uint32_t *ok_count,
                 uint32_t *fail_count);

size_t alt_diag_count(void);

// Samples the RX pin BEFORE the UART driver takes it over, to tell a connected
// line from a dead one. An idle UART TX holds its line high; a low reading
// means GPIO16 is not reaching anything that drives it. Call once, first thing,
// before alt_serial_init().
void alt_probe_rx_idle(void);

// Human-readable result of the above ("idle high (line driven)", ...).
const char *alt_rx_idle_state(void);

// When the reading above was taken: "boot" or "on demand".
const char *alt_rx_idle_when(void);

// Re-samples the RX pin while the UART driver still owns it, so a wire can be
// moved and re-checked without power-cycling the heat pump. Reads the pad level
// directly; the UART routing is left untouched.
void alt_resample_rx(void);

// Sweeps both protocols across their common registries and records every
// outcome in the diagnostics table, so "is this machine protocol I or S" can be
// answered from the web UI without a cable. Runs in its own task; returns
// immediately. Poll the diagnostics to see results appear.
void alt_probe_start(void);

// True while a sweep is running.
bool alt_probe_running(void);
