// Ported from upstream ESPAltherma include/comm.h (MIT, (c) 2020 Raomin).
// The framing, reply-length rules and CRC are field-proven; they are carried
// over literally. What changed is only the plumbing: HardwareSerial ->
// driver/uart.h, millis() deadlines -> esp_timer, Serial.printf -> ESP_LOGx.

#include "althermaserial.h"

#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_config.h"

static const char *TAG = "x10a";

static SemaphoreHandle_t s_uart_lock;
static int query_registry_locked(uint8_t reg_id, uint8_t *buf, char protocol);

// Set by the last query, so the register scan can tell the failure modes apart
// without duplicating the parsing.
static bool s_last_was_not_impl;   // machine answered 0x15 0xEA
static int  s_last_bytes;          // bytes received, whatever the outcome

// Scan knobs. The scan walks 256 IDs, so it must not evict the poll loop's
// diagnostics, and it cannot afford the usual half-second settle after each
// silent register (that alone would add two minutes).
static bool s_diag_suppress;
static int  s_fail_settle_ms = 500;

// ---- link diagnostics, readable over HTTP (see althermaserial.h) --------

typedef struct {
    uint8_t  reg_id;
    char     proto;
    bool     used;
    char     status[64];
    char     hex[ALT_REPLY_MAX * 5 + 1];
    int      bytes;
    uint32_t ok_count;
    uint32_t fail_count;
} alt_diag_t;

static alt_diag_t s_diag[ALT_DIAG_MAX];

static alt_diag_t *diag_for(uint8_t reg_id, char proto)
{
    for (size_t i = 0; i < ALT_DIAG_MAX; i++) {
        if (s_diag[i].used && s_diag[i].reg_id == reg_id && s_diag[i].proto == proto) {
            return &s_diag[i];
        }
    }
    for (size_t i = 0; i < ALT_DIAG_MAX; i++) {
        if (!s_diag[i].used) {
            s_diag[i].used = true;
            s_diag[i].reg_id = reg_id;
            s_diag[i].proto = proto;
            return &s_diag[i];
        }
    }
    return NULL;
}

static void diag_record(uint8_t reg_id, char proto, const char *status,
                        const uint8_t *buf, int len, bool ok)
{
    if (s_diag_suppress) {
        return;
    }
    alt_diag_t *d = diag_for(reg_id, proto);
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

bool alt_diag_at(size_t i, uint8_t *reg_id, char *protocol, const char **status,
                 const char **hex, int *bytes, uint32_t *ok_count,
                 uint32_t *fail_count)
{
    if (i >= ALT_DIAG_MAX || !s_diag[i].used) {
        return false;
    }
    if (reg_id)     *reg_id     = s_diag[i].reg_id;
    if (protocol)   *protocol   = s_diag[i].proto;
    if (status)     *status     = s_diag[i].status;
    if (hex)        *hex        = s_diag[i].hex;
    if (bytes)      *bytes      = s_diag[i].bytes;
    if (ok_count)   *ok_count   = s_diag[i].ok_count;
    if (fail_count) *fail_count = s_diag[i].fail_count;
    return true;
}

// ---- RX line check ------------------------------------------------------

static char s_rx_idle[48] = "not sampled";
static char s_rx_when[16] = "never";

const char *alt_rx_idle_state(void)
{
    return s_rx_idle;
}

const char *alt_rx_idle_when(void)
{
    return s_rx_when;
}

// Counts high samples over ~50 ms - several character times at 9600 baud, so
// real traffic shows up as toggling rather than a clean level.
static void classify_rx(void)
{
    int highs = 0;
    const int samples = 200;
    for (int i = 0; i < samples; i++) {
        highs += gpio_get_level(ALT_UART_RX_PIN);
        esp_rom_delay_us(250);
    }

    if (highs == samples) {
        strlcpy(s_rx_idle, "idle high (line driven)", sizeof(s_rx_idle));
    } else if (highs == 0) {
        strlcpy(s_rx_idle, "idle LOW (not driven?)", sizeof(s_rx_idle));
    } else {
        snprintf(s_rx_idle, sizeof(s_rx_idle), "toggling (%d%% high, traffic?)",
                 highs * 100 / samples);
    }
}

void alt_probe_rx_idle(void)
{
    // Sampled before uart_set_pin() claims the pin. An idle UART TX on the far
    // end holds the line HIGH, so a steady low means GPIO16 is not connected to
    // anything driving it (or is shorted to ground). No pull is enabled: a pull
    // would manufacture the very level we are trying to measure.
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << ALT_UART_RX_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        strlcpy(s_rx_idle, "sample failed", sizeof(s_rx_idle));
        return;
    }

    classify_rx();
    strlcpy(s_rx_when, "boot", sizeof(s_rx_when));
    ESP_LOGI(TAG, "RX GPIO%d before UART init: %s", ALT_UART_RX_PIN, s_rx_idle);
}

void alt_resample_rx(void)
{
    // No gpio_config here: uart_set_pin() already enabled the pad input, and
    // gpio_get_level() reads the pad regardless of the GPIO-matrix routing. So
    // the level can be re-read without disturbing the UART - which means a wire
    // can be moved and re-checked without power-cycling the heat pump.
    //
    // The mutex keeps this out of the middle of a query: our own TX does not
    // affect RX, but a sample taken while the pump is mid-reply would report
    // "toggling" and confuse rather than inform.
    if (s_uart_lock) {
        xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    }
    classify_rx();
    strlcpy(s_rx_when, "on demand", sizeof(s_rx_when));
    if (s_uart_lock) {
        xSemaphoreGive(s_uart_lock);
    }
    ESP_LOGI(TAG, "RX GPIO%d re-sampled: %s", ALT_UART_RX_PIN, s_rx_idle);
}

// ---- protocol sweep -----------------------------------------------------

static volatile bool s_probe_running;

bool alt_probe_running(void)
{
    return s_probe_running;
}

static void probe_task(void *arg)
{
    (void)arg;
    // Registries each protocol is known to answer. If the machine speaks the
    // other dialect, its own set replies and ours stays silent - which is the
    // whole point of asking both.
    static const uint8_t regs_i[] = {0x10, 0x20, 0x21, 0x60, 0x61};
    static const uint8_t regs_s[] = {0x50, 0x53, 0x54, 0x55, 0x56};
    uint8_t buf[ALT_REPLY_MAX];

    ESP_LOGI(TAG, "protocol sweep starting");
    for (size_t i = 0; i < sizeof(regs_i) / sizeof(regs_i[0]); i++) {
        alt_query_registry(regs_i[i], buf, 'I');
    }
    for (size_t i = 0; i < sizeof(regs_s) / sizeof(regs_s[0]); i++) {
        alt_query_registry(regs_s[i], buf, 'S');
    }
    ESP_LOGI(TAG, "protocol sweep done");

    s_probe_running = false;
    vTaskDelete(NULL);
}

void alt_probe_start(void)
{
    if (s_probe_running) {
        return;
    }
    s_probe_running = true;
    xTaskCreate(&probe_task, "x10a_probe", 4096, NULL, 4, NULL);
}

// ---- full register scan -------------------------------------------------

static volatile bool s_scan_running;
static volatile int  s_scan_progress;
static uint8_t s_scan_outcome[256];

typedef struct {
    uint8_t  reg_id;
    int      bytes;
    char     hex[ALT_REPLY_MAX * 5 + 1];
    alt_scan_outcome_t outcome;
} alt_scan_hit_t;

static alt_scan_hit_t s_scan_hits[ALT_SCAN_HITS_MAX];
static size_t s_scan_hit_count;

bool alt_scan_running(void)  { return s_scan_running; }
int  alt_scan_progress(void) { return s_scan_progress; }
size_t alt_scan_hit_count(void) { return s_scan_hit_count; }

void alt_scan_totals(int *ok, int *not_impl, int *bad_crc, int *silent)
{
    int o = 0, n = 0, b = 0, s = 0;
    for (int i = 0; i < 256; i++) {
        switch (s_scan_outcome[i]) {
        case ALT_SCAN_OK:       o++; break;
        case ALT_SCAN_NOT_IMPL: n++; break;
        case ALT_SCAN_BAD_CRC:  b++; break;
        case ALT_SCAN_SILENT:   s++; break;
        default: break;
        }
    }
    if (ok)       *ok = o;
    if (not_impl) *not_impl = n;
    if (bad_crc)  *bad_crc = b;
    if (silent)   *silent = s;
}

bool alt_scan_hit_at(size_t i, uint8_t *reg_id, const char **hex, int *bytes,
                     alt_scan_outcome_t *outcome)
{
    if (i >= s_scan_hit_count) {
        return false;
    }
    if (reg_id)  *reg_id  = s_scan_hits[i].reg_id;
    if (hex)     *hex     = s_scan_hits[i].hex;
    if (bytes)   *bytes   = s_scan_hits[i].bytes;
    if (outcome) *outcome = s_scan_hits[i].outcome;
    return true;
}

static void scan_task(void *arg)
{
    char protocol = (char)(intptr_t)arg;
    uint8_t buf[ALT_REPLY_MAX];

    memset(s_scan_outcome, 0, sizeof(s_scan_outcome));
    s_scan_hit_count = 0;
    s_scan_progress = 0;

    // Results go to the scan table, not the per-registry diagnostics: 256 IDs
    // would evict everything the poll loop has recorded.
    s_diag_suppress = true;
    // A silent register costs the full reply timeout; the usual half-second
    // settle after a failure would add two minutes across 256 of them.
    s_fail_settle_ms = 100;

    ESP_LOGI(TAG, "scanning all 256 registry IDs on protocol '%c'", protocol);

    for (int reg = 0; reg < 256; reg++) {
        int len = alt_query_registry((uint8_t)reg, buf, protocol);

        alt_scan_outcome_t outcome;
        if (len > 0) {
            outcome = ALT_SCAN_OK;
        } else if (s_last_was_not_impl) {
            outcome = ALT_SCAN_NOT_IMPL;
        } else if (s_last_bytes > 0) {
            outcome = ALT_SCAN_BAD_CRC;
        } else {
            outcome = ALT_SCAN_SILENT;
        }
        s_scan_outcome[reg] = (uint8_t)outcome;

        // Keep the raw bytes for anything that actually answered - those are
        // the interesting ones, and there are few enough to store.
        if ((outcome == ALT_SCAN_OK || outcome == ALT_SCAN_BAD_CRC) &&
            s_scan_hit_count < ALT_SCAN_HITS_MAX) {
            alt_scan_hit_t *h = &s_scan_hits[s_scan_hit_count++];
            h->reg_id = (uint8_t)reg;
            h->bytes = len > 0 ? len : s_last_bytes;
            h->outcome = outcome;
            alt_format_buffer(buf, (size_t)h->bytes, h->hex, sizeof(h->hex));
            ESP_LOGW(TAG, "scan: 0x%02x ANSWERED (%d bytes) %s",
                     reg, h->bytes, h->hex);
        }

        s_scan_progress = reg + 1;
    }

    s_fail_settle_ms = 500;
    s_diag_suppress = false;

    int ok = 0, ni = 0, bad = 0, sil = 0;
    alt_scan_totals(&ok, &ni, &bad, &sil);
    ESP_LOGI(TAG, "scan done: %d ok, %d not-implemented, %d bad CRC, %d silent",
             ok, ni, bad, sil);

    s_scan_running = false;
    vTaskDelete(NULL);
}

void alt_scan_start(char protocol)
{
    if (s_scan_running || s_probe_running) {
        return;
    }
    s_scan_running = true;
    xTaskCreate(&scan_task, "x10a_scan", 4096, (void *)(intptr_t)protocol, 4, NULL);
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

    s_uart_lock = xSemaphoreCreateMutex();

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
    // The poll loop and the sweep task both reach this, and two interleaved
    // queries on one UART would corrupt each other's replies.
    if (s_uart_lock) {
        xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    }
    int result = query_registry_locked(reg_id, buf, protocol);
    if (s_uart_lock) {
        xSemaphoreGive(s_uart_lock);
    }
    return result;
}

static int query_registry_locked(uint8_t reg_id, uint8_t *buf, char protocol)
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
    s_last_was_not_impl = false;
    s_last_bytes = 0;

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
            s_last_was_not_impl = true;
            s_last_bytes = len;
            diag_record(reg_id, protocol, "HP error 0x15 0xEA", buf, len, false);
            vTaskDelay(pdMS_TO_TICKS(s_fail_settle_ms));
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
        s_last_bytes = len;
        diag_record(reg_id, protocol, status, buf, len, false);
        vTaskDelay(pdMS_TO_TICKS(s_fail_settle_ms));
        return -1;
    }

    alt_format_buffer(buf, len, hex, sizeof(hex));

    uint8_t want = alt_crc(buf, len - 1);
    if (want != buf[len - 1]) {
        ESP_LOGW(TAG, "reg 0x%02x: bad CRC, calculated 0x%02x but got 0x%02x: %s",
                 reg_id, want, buf[len - 1], hex);
        s_last_bytes = len;
        char status[64];
        snprintf(status, sizeof(status), "bad CRC: want 0x%02x got 0x%02x",
                 want, buf[len - 1]);
        diag_record(reg_id, protocol, status, buf, len, false);
        return -1;
    }

    ESP_LOGI(TAG, "reg 0x%02x: %s (CRC OK)", reg_id, hex);
    diag_record(reg_id, protocol, "ok", buf, len, true);
    return len;
}
