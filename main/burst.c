// Burst sampling - see burst.h for why this is a bounded window and not a
// poll-interval setting.

#include "burst.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "converters.h"
#include "derived.h"

static const char *TAG = "burst";

// The inputs worth capturing at this rate: the boolean register and the water
// temperatures. Located by registry and offset for the same reason derived.c
// does it - a label is user-facing text, an offset is the physical thing.
static const struct {
    uint8_t reg;
    int     offset;
    const char *column;
} kFields[] = {
    {0x53, 0,  "pump"},
    {0x53, 3,  "heater"},
    {0x53, 5,  "dhw"},
    {0x54, 0,  "refrig"},
    {0x54, 2,  "inlet"},
    {0x54, 4,  "outlet"},
    {0x54, 8,  "tank"},
};
#define FIELD_COUNT (sizeof(kFields) / sizeof(kFields[0]))

// Values are short - "ON", "OFF", "31.0625". The converter's own buffer is 30
// bytes; 12 holds every reading this unit produces and keeps the ring small.
#define VALUE_MAX 12

typedef struct {
    uint32_t ms;                          // since boot, at the end of the cycle
    char     value[FIELD_COUNT][VALUE_MAX];
    char     compressor[4];               // "ON" / "OFF" / ""
} sample_t;

static sample_t s_ring[ALT_BURST_MAX_SAMPLES];
static size_t   s_count;
static bool     s_overflow;
static int64_t  s_deadline_us;
static TaskHandle_t s_waiter;

void alt_burst_register_waiter(TaskHandle_t task)
{
    s_waiter = task;
}

esp_err_t alt_burst_start(int seconds)
{
    if (seconds < 1 || seconds > ALT_BURST_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (alt_burst_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_count = 0;
    s_overflow = false;
    // Set last: the poll loop reads the deadline without a lock, so the buffer
    // must already be reset when the window opens.
    s_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGI(TAG, "sampling as fast as the link allows for %d s", seconds);

    // Cut the poll loop's wait short so the window starts now rather than at
    // the next scheduled cycle.
    if (s_waiter) {
        xTaskNotifyGive(s_waiter);
    }
    return ESP_OK;
}

bool alt_burst_active(void)
{
    return s_deadline_us != 0 && esp_timer_get_time() < s_deadline_us;
}

void alt_burst_record(void)
{
    if (s_count >= ALT_BURST_MAX_SAMPLES) {
        if (!s_overflow) {
            ESP_LOGW(TAG, "buffer full at %d samples, dropping the rest",
                     ALT_BURST_MAX_SAMPLES);
            s_overflow = true;
        }
        return;
    }

    sample_t *s = &s_ring[s_count];
    s->ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (size_t f = 0; f < FIELD_COUNT; f++) {
        const char *value = NULL;
        if (!converter_label_by_offset(kFields[f].reg, kFields[f].offset, NULL, &value)) {
            value = "";
        }
        snprintf(s->value[f], VALUE_MAX, "%s", value);
    }
    snprintf(s->compressor, sizeof(s->compressor), "%s", alt_derived_compressor());

    s_count++;
}

size_t alt_burst_count(void)
{
    return s_count;
}

bool alt_burst_overflowed(void)
{
    return s_overflow;
}

const char *alt_burst_csv_header(void)
{
    return "ms,pump,heater,dhw,refrig,inlet,outlet,tank,compressor";
}

bool alt_burst_row(size_t i, char *out, size_t out_len)
{
    if (i >= s_count) {
        return false;
    }
    const sample_t *s = &s_ring[i];
    int n = snprintf(out, out_len, "%lu", (unsigned long)s->ms);
    for (size_t f = 0; f < FIELD_COUNT && n > 0 && (size_t)n < out_len; f++) {
        n += snprintf(out + n, out_len - n, ",%s", s->value[f]);
    }
    if (n > 0 && (size_t)n < out_len) {
        snprintf(out + n, out_len - n, ",%s", s->compressor);
    }
    return true;
}
