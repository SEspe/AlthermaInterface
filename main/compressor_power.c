// Compressor state from outdoor-unit current - see compressor_power.h for why.

#include "compressor_power.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "settings.h"

static const char *TAG = "comp_pwr";

// Poll rate. The point of this source is timing, so it is polled far faster
// than the X10A cycle: at 5 s the edge is known to within 5 s, against 39 s
// measured for the water delta. Fast enough to matter, slow enough that the
// PowerMeter is serving one small request every five seconds.
#define POLL_MS         5000

// A reading older than this is refused. Three missed polls, so one timeout or
// a brief reboot does not drop the source, but a node that has stopped
// answering does.
#define STALE_S         20

// Plausibility bound. This measures one supply to one outdoor unit; tens of
// amps would mean the channel is measuring something else, or the scale is
// wrong. Refusing it is what would have caught the floating CT input on
// 2026-09-15, which reported a steady 3.9 A while the compressor was off -
// though note that reading was *inside* this bound, so the bound is a backstop
// and not a substitute for checking the instrument.
#define MAX_PLAUSIBLE_A 60.0f

// Response cap. The JSON is a few hundred bytes; anything larger is not the
// endpoint we think it is.
#define RESP_MAX        2048

// Counters and the last failure reason, for the Debug tab. Written only by the
// poll task and read without a lock: each is a single word, the reader wants a
// snapshot rather than a consistent set, and a torn count is worth less than
// the lock would cost on the polling path.
static uint32_t     s_polls, s_ok, s_fail;
static alt_cp_err_t s_last_err = ALT_CP_ERR_DISABLED;
static int          s_http_status;

static alt_cp_state_t s_state = ALT_CP_UNKNOWN;
static float          s_amps;
static int64_t        s_last_us;      // 0 = never read
static bool           s_on;           // hysteresis memory
static TaskHandle_t   s_waiter;

// Pulls "i" out of the channel object whose "label" matches. The response is
// {"ch":[{"label":"Outdoor","i":7.64,...},...]}, and a PowerMeter may carry
// several channels, so the label is what selects ours. Parsed by scanning
// rather than with a JSON library: one field out of a known shape does not
// justify pulling cJSON into this component.
static bool parse_channel_amps(const char *body, const char *label, float *out)
{
    char needle[96];
    snprintf(needle, sizeof(needle), "\"label\":\"%s\"", label);

    const char *p = strstr(body, needle);
    if (!p) {
        return false;
    }
    // "i" is the first field after the label in every response seen, but do
    // not rely on that - search forward for it explicitly, and stop at the end
    // of this channel object so a later channel's current cannot be read.
    const char *end = strchr(p, '}');
    const char *i = strstr(p, "\"i\":");
    if (!i || (end && i > end)) {
        return false;
    }
    char *stop = NULL;
    float v = strtof(i + 4, &stop);
    if (stop == i + 4) {
        return false;
    }
    *out = v;
    return true;
}

static bool fetch_amps(float *out)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s/api/values", alt_settings_pm_host());

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }

    bool ok = false;
    char *body = NULL;

    s_http_status = 0;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "open %s: %s", url, esp_err_to_name(err));
        s_last_err = ALT_CP_ERR_CONNECT;
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        s_last_err = ALT_CP_ERR_CONNECT;
        goto done;
    }
    s_http_status = esp_http_client_get_status_code(c);
    if (s_http_status != 200) {
        s_last_err = ALT_CP_ERR_HTTP;
        goto done;
    }

    body = malloc(RESP_MAX);
    if (!body) {
        s_last_err = ALT_CP_ERR_EMPTY;
        goto done;
    }
    int n = esp_http_client_read_response(c, body, RESP_MAX - 1);
    if (n <= 0) {
        s_last_err = ALT_CP_ERR_EMPTY;
        goto done;
    }
    body[n] = '\0';

    ok = parse_channel_amps(body, alt_settings_pm_channel(), out);
    if (!ok) {
        ESP_LOGW(TAG, "channel \"%s\" not found in response",
                 alt_settings_pm_channel());
        s_last_err = ALT_CP_ERR_CHANNEL;
    }

done:
    free(body);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static void poll_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "polling %s channel \"%s\" every %d s, on >= %.2f A, off < %.2f A",
             alt_settings_pm_host(), alt_settings_pm_channel(), POLL_MS / 1000,
             alt_settings_pm_on_amps(), alt_settings_pm_off_amps());

    while (true) {
        float a = 0.0f;
        s_polls++;
        bool got = fetch_amps(&a);
        if (got && (a < 0.0f || a > MAX_PLAUSIBLE_A)) {
            s_last_err = ALT_CP_ERR_IMPLAUSIBLE;
            got = false;
        }
        if (got) {
            s_ok++;
            s_last_err = ALT_CP_ERR_NONE;
            s_amps = a;
            s_last_us = esp_timer_get_time();
            s_on = s_on ? (a >= alt_settings_pm_off_amps())
                        : (a >= alt_settings_pm_on_amps());
            alt_cp_state_t now = s_on ? ALT_CP_ON : ALT_CP_OFF;
            if (now != s_state) {
                s_state = now;
                ESP_LOGI(TAG, "compressor %s (%.2f A)", s_on ? "ON" : "OFF", a);
                // Publish this edge now rather than at the next scheduled
                // cycle. The poll loop re-reads the registries and publishes,
                // which costs one extra X10A cycle per transition - a handful
                // an hour against the 120 it already does.
                if (s_waiter) {
                    xTaskNotifyGive(s_waiter);
                }
            }
        } else {
            s_fail++;
            if (s_last_us != 0 &&
                (esp_timer_get_time() - s_last_us) > (int64_t)STALE_S * 1000000) {
                // Do not keep publishing the last known state indefinitely.
                // Going UNKNOWN hands the decision back to the water delta,
                // which is always available.
                if (s_state != ALT_CP_UNKNOWN) {
                    ESP_LOGW(TAG, "no reading for %d s, falling back to the water delta",
                             STALE_S);
                }
                s_state = ALT_CP_UNKNOWN;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void alt_compressor_power_register_waiter(TaskHandle_t task)
{
    s_waiter = task;
}

void alt_compressor_power_start(void)
{
    if (strlen(alt_settings_pm_host()) == 0) {
        ESP_LOGI(TAG, "no PowerMeter configured; compressor state comes from "
                      "the water delta alone");
        return;
    }
    // 4 KB holds the HTTP client plus the response buffer. The register scan
    // panicked at 4 KB once because it also carried the MQTT log hook's line
    // buffer; this task logs almost nothing, so it does not.
    xTaskCreate(&poll_task, "comp_pwr", 4096, NULL, 4, NULL);
}

alt_cp_state_t alt_compressor_power_state(void)
{
    if (s_last_us == 0) {
        return ALT_CP_UNKNOWN;
    }
    if ((esp_timer_get_time() - s_last_us) > (int64_t)STALE_S * 1000000) {
        return ALT_CP_UNKNOWN;
    }
    return s_state;
}

bool alt_compressor_power_reading(float *amps, int *age_s)
{
    if (s_last_us == 0) {
        return false;
    }
    if (amps)  *amps  = s_amps;
    if (age_s) *age_s = (int)((esp_timer_get_time() - s_last_us) / 1000000);
    return true;
}

const char *alt_compressor_power_err_name(alt_cp_err_t e)
{
    switch (e) {
    case ALT_CP_ERR_NONE:        return "ok";
    case ALT_CP_ERR_DISABLED:    return "no host configured";
    case ALT_CP_ERR_CONNECT:     return "unreachable";
    case ALT_CP_ERR_HTTP:        return "bad HTTP status";
    case ALT_CP_ERR_EMPTY:       return "empty response";
    case ALT_CP_ERR_CHANNEL:     return "channel not found";
    case ALT_CP_ERR_IMPLAUSIBLE: return "reading out of range";
    }
    return "?";
}

void alt_compressor_power_stats(alt_cp_stats_t *out)
{
    if (!out) {
        return;
    }
    out->enabled     = strlen(alt_settings_pm_host()) > 0;
    out->state       = alt_compressor_power_state();
    out->amps        = s_amps;
    out->age_s       = s_last_us ? (int)((esp_timer_get_time() - s_last_us) / 1000000)
                                 : -1;
    out->polls       = s_polls;
    out->ok          = s_ok;
    out->fail        = s_fail;
    out->last_err    = s_last_err;
    out->http_status = s_http_status;
}
