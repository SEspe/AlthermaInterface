// Derived values - see derived.h for why the compressor has to be inferred.

#include "derived.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "converters.h"

static const char *TAG = "derived";

// Inputs, by registry and payload offset rather than by label - the labels are
// user-facing text and keying on them is the trap docs/REGISTER_0x53.md
// describes. These three are from main/def/EKHBH008BA.h.
#define REG_BOOLS       0x53
#define OFF_PUMP        0
#define REG_TEMPS       0x54
#define OFF_INLET       2
#define OFF_OUTLET      4

// Offsets alone would silently produce nonsense if a different definition file
// were selected in model_config.h - plain PROTOCOL_S.h puts the indoor heat
// exchanger at 0x54 offset 2, not inlet water. So the label is checked once as
// a guard: not to find the value, but to confirm the position still means what
// this rule assumes. A mismatch disables the sensor loudly instead of
// publishing a confident lie.
#define GUARD_PUMP      "pump"
#define GUARD_INLET     "Inlet water"
#define GUARD_OUTLET    "Outlet Water"

// Thresholds in kelvin across the plate heat exchanger, with hysteresis.
//
// Evidence: three captures in captures/, 700+ samples over a forced DHW cycle,
// an overnight run and a space-heating run. With the pump circulating, the
// water delta is sharply bimodal - compressor off sits at 0.1-0.9 K, compressor
// running at 3-7 K - and almost nothing is recorded in between (8 of 426
// pump-on samples land in 1-3 K, all of them during a transition). The
// refrigerant liquid side agrees independently: about 5-6 K below inlet water
// when idle, level with it when running.
//
// The gap is wide, so the exact numbers are not critical; the hysteresis is
// here to keep a machine that parks on a threshold from chattering rather than
// because the data is noisy.
#define COMP_ON_DELTA_K   2.0
#define COMP_OFF_DELTA_K  1.2

static const char *s_compressor = "";
static bool s_compressor_on;
static bool s_guard_failed;

// Reads the label at (reg, offset), checking that its name still contains
// `guard`. Returns NULL when the label is missing, renamed, or not yet read.
static const char *input_value(uint8_t reg, int offset, const char *guard)
{
    const char *label = NULL;
    const char *value = NULL;

    if (!converter_label_by_offset(reg, offset, &label, &value)) {
        if (!s_guard_failed) {
            ESP_LOGE(TAG, "no label at 0x%02x offset %d; compressor state "
                          "unavailable", reg, offset);
            s_guard_failed = true;
        }
        return NULL;
    }
    if (strstr(label, guard) == NULL) {
        if (!s_guard_failed) {
            ESP_LOGE(TAG, "0x%02x offset %d is \"%s\", expected something "
                          "containing \"%s\"; compressor state unavailable",
                     reg, offset, label, guard);
            s_guard_failed = true;
        }
        return NULL;
    }
    return value[0] != '\0' ? value : NULL;
}

// strtod that rejects trailing junk, so a converter error string can never be
// read as a temperature.
static bool parse_number(const char *s, double *out)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || (end && *end != '\0')) {
        return false;
    }
    *out = v;
    return true;
}

void alt_derived_update(void)
{
    const char *pump   = input_value(REG_BOOLS, OFF_PUMP,   GUARD_PUMP);
    const char *inlet  = input_value(REG_TEMPS, OFF_INLET,  GUARD_INLET);
    const char *outlet = input_value(REG_TEMPS, OFF_OUTLET, GUARD_OUTLET);

    if (!pump || !inlet || !outlet) {
        s_compressor = "";
        s_compressor_on = false;
        return;
    }

    // No flow means the water delta measures nothing - stagnant water has shown
    // a 9.9 K spread with the machine winding down. The compressor has never
    // been observed running with the pump stopped, and on a hydrobox it cannot:
    // it would have no heat sink.
    if (strcmp(pump, "ON") != 0) {
        s_compressor = "OFF";
        s_compressor_on = false;
        return;
    }

    double in = 0.0, out = 0.0;
    if (!parse_number(inlet, &in) || !parse_number(outlet, &out)) {
        ESP_LOGW(TAG, "water temps not numeric (\"%s\", \"%s\")", inlet, outlet);
        s_compressor = "";
        s_compressor_on = false;
        return;
    }

    const double delta = out - in;
    if (s_compressor_on) {
        s_compressor_on = delta > COMP_OFF_DELTA_K;
    } else {
        s_compressor_on = delta >= COMP_ON_DELTA_K;
    }
    s_compressor = s_compressor_on ? "ON" : "OFF";
}

const char *alt_derived_compressor(void)
{
    return s_compressor;
}
