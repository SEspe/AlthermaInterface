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
