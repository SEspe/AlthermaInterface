// Ported from upstream ESPAltherma include/comm.h (MIT, (c) 2020 Raomin).
// The framing, reply-length rules and CRC are field-proven; they are carried
// over literally. What changed is only the plumbing: HardwareSerial ->
// driver/uart.h, millis() deadlines -> esp_timer, Serial.printf -> ESP_LOGx.

#include "althermaserial.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"

static const char *TAG = "x10a";

// ---- link diagnostics, readable over HTTP (see althermaserial.h) --------

typedef struct {
    uint8_t  reg_id;
    bool     used;
    char     status[64];
    char     hex[ALT_REPLY_MAX * 5 + 1];
    int      bytes;
    uint32_t ok_count;
    uint32_t fail_count;
} alt_diag_t;

static alt_diag_t s_diag[ALT_DIAG_MAX];

static alt_diag_t *diag_for(uint8_t reg_id)
{
    for (size_t i = 0; i < ALT_DIAG_MAX; i++) {
        if (s_diag[i].used && s_diag[i].reg_id == reg_id) {
            return &s_diag[i];
        }
    }
    for (size_t i = 0; i < ALT_DIAG_MAX; i++) {
        if (!s_diag[i].used) {
            s_diag[i].used = true;
            s_diag[i].reg_id = reg_id;
            return &s_diag[i];
        }
    }
    return NULL;
}

static void diag_record(uint8_t reg_id, const char *status, const uint8_t *buf,
                        int len, bool ok)
{
    alt_diag_t *d = diag_for(reg_id);
    if (!d) {
        return;
    }
    strlcpy(d->status, status, sizeof(d->status));
    d->bytes = len;
    if (len > 0) {
        alt_format_buffer(buf, (size_t)len, d->hex, sizeof(d->hex));
    } else {
        d->hex[0] = '\0';
    }
    if (ok) {
        d->ok_count++;
    } else {
        d->fail_count++;
    }
}

size_t alt_diag_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < ALT_DIAG_MAX; i++) {
        if (s_diag[i].used) {
            n++;
        }
    }
    return n;
}

bool alt_diag_at(size_t i, uint8_t *reg_id, const char **status,
                 const char **hex, int *bytes, uint32_t *ok_count,
                 uint32_t *fail_count)
{
    if (i >= ALT_DIAG_MAX || !s_diag[i].used) {
        return false;
    }
    if (reg_id)     *reg_id     = s_diag[i].reg_id;
    if (status)     *status     = s_diag[i].status;
    if (hex)        *hex        = s_diag[i].hex;
    if (bytes)      *bytes      = s_diag[i].bytes;
    if (ok_count)   *ok_count   = s_diag[i].ok_count;
    if (fail_count) *fail_count = s_diag[i].fail_count;
    return true;
}

uint8_t alt_crc(const uint8_t *src, size_t len)
{
    uint8_t b = 0;
    for (size_t i = 0; i < len; i++) {
        b += src[i];
    }
    return ~b;
}

void alt_format_buffer(const uint8_t *buf, size_t len, char *out, size_t out_len)
{
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len && pos + 6 < out_len; i++) {
        pos += snprintf(out + pos, out_len - pos, "0x%02x ", buf[i]);
    }
}

esp_err_t alt_serial_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = ALT_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,   // 8E1, both protocols
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(ALT_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(ALT_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(ALT_UART_PORT, ALT_UART_TX_PIN, ALT_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "UART%d up: RX=GPIO%d TX=GPIO%d, %d 8E1",
             (int)ALT_UART_PORT, ALT_UART_RX_PIN, ALT_UART_TX_PIN, ALT_UART_BAUD);
    return ESP_OK;
}

// Protocol I returns the real length in byte 3 of the response; the 12 here is
// only the initial read budget. Protocol S lengths are fixed per registry.
// NOTE: upstream's doc/Daikin S protocol.md says 0x56 replies with 6 bytes
// while its code says 4. The code is what runs against real hardware, so it
// wins; 0x56 is not queried by PROTOCOL_S.h anyway.
static int reply_len_for(uint8_t reg_id, char protocol)
{
    if (protocol == 'I') {
        return 12;
    }
    switch (reg_id) {
    case 0x50: return 6;
    case 0x56: return 4;
    default:   return 18;
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int alt_query_registry(uint8_t reg_id, uint8_t *buf, char protocol)
{
    uint8_t prep[4] = {0x03, 0x40, reg_id, 0x00};
    int query_len = 4;

    if (protocol == 'S') {
        prep[0] = 0x02;
        prep[1] = reg_id;
        prep[2] = alt_crc(prep, 2);
        prep[3] = 0;
        query_len = 3;
    } else {
        prep[3] = alt_crc(prep, 3);
    }

    memset(buf, 0, ALT_REPLY_MAX);

    // Drop anything still sitting in the RX FIFO, or a stale byte shifts the
    // whole reply and every value decodes as garbage.
    uart_flush_input(ALT_UART_PORT);
    uart_write_bytes(ALT_UART_PORT, prep, query_len);

    const int64_t start = now_ms();
    int len = 0;
    int reply_len = reply_len_for(reg_id, protocol);
    char hex[ALT_REPLY_MAX * 5 + 1];

    while (len < reply_len) {
        int64_t remaining = ALT_UART_TIMEOUT_MS - (now_ms() - start);
        if (remaining <= 0) {
            break;
        }
        uint8_t byte;
        int got = uart_read_bytes(ALT_UART_PORT, &byte, 1, pdMS_TO_TICKS(remaining));
        if (got != 1) {
            continue;
        }
        buf[len++] = byte;

        if (protocol == 'I' && len == 3) {
            // Real length lives in byte 3, not counting the bytes already read.
            reply_len = buf[2] + 2;
            if (reply_len > ALT_REPLY_MAX) {
                reply_len = ALT_REPLY_MAX;
            }
        }
        // Error reply, common to both protocols.
        if (len == 2 && buf[0] == 0x15 && buf[1] == 0xea) {
            ESP_LOGW(TAG, "reg 0x%02x: HP returned 0x15 0xEA (command not understood)", reg_id);
            diag_record(reg_id, "HP error 0x15 0xEA", buf, len, false);
            vTaskDelay(pdMS_TO_TICKS(500));
            return -1;
        }
    }

    if (len < reply_len) {
        char status[64];
        if (len == 0) {
            ESP_LOGW(TAG, "reg 0x%02x: timeout, no bytes. Check wiring/pins.", reg_id);
            strlcpy(status, "timeout: no bytes", sizeof(status));
        } else {
            alt_format_buffer(buf, len, hex, sizeof(hex));
            ESP_LOGW(TAG, "reg 0x%02x: timeout, got %d/%d bytes: %s",
                     reg_id, len, reply_len, hex);
            snprintf(status, sizeof(status), "timeout: %d of %d bytes", len, reply_len);
        }
        diag_record(reg_id, status, buf, len, false);
        vTaskDelay(pdMS_TO_TICKS(500));
        return -1;
    }

    alt_format_buffer(buf, len, hex, sizeof(hex));

    uint8_t want = alt_crc(buf, len - 1);
    if (want != buf[len - 1]) {
        ESP_LOGW(TAG, "reg 0x%02x: bad CRC, calculated 0x%02x but got 0x%02x: %s",
                 reg_id, want, buf[len - 1], hex);
        char status[64];
        snprintf(status, sizeof(status), "bad CRC: want 0x%02x got 0x%02x",
                 want, buf[len - 1]);
        diag_record(reg_id, status, buf, len, false);
        return -1;
    }

    ESP_LOGI(TAG, "reg 0x%02x: %s (CRC OK)", reg_id, hex);
    diag_record(reg_id, "ok", buf, len, true);
    return len;
}
