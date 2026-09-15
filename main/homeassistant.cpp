// Ported from upstream ESPAltherma src/homeassistant.cpp
// (MIT, (c) 2020 Raomin).
//
// The payload shape, the entity naming rule and the device-class heuristics are
// upstream's and are kept verbatim - an existing Home Assistant setup must not
// see different entity IDs after switching firmware.
//
// Two structural changes:
//
// 1. Upstream streams the payload straight to the socket via
//    PubSubClient::beginPublish(), because a protocol-I definition can carry
//    hundreds of labels and the buffer would not fit in heap. esp-mqtt has no
//    streaming publish, so the payload is materialised here instead. That is
//    affordable for this unit (26 labels, ~7 KB); a protocol-I machine would
//    need cfg.buffer.size in mqtt.c raised to match.
//
// 2. Labels are read through the converters C facade rather than by including
//    a definition header. The definition headers DEFINE labelDefs[], so
//    including one from a second translation unit is a duplicate-symbol link
//    error - which is exactly what happened when this file included
//    model_config.h alongside converters.cpp.

#include "homeassistant.h"

#include <cctype>
#include <cstring>
#include <string>

#include "esp_log.h"

#include "converters.h"
#include "derived.h"
#include "version.h"

static const char *TAG = "ha";

// Device identity is upstream's, deliberately: the same ids mean a machine that
// once ran upstream's firmware keeps its Home Assistant device and history.
static const char *kDiscoveryStart =
    "{\"dev\":{\"ids\":\"espaltherma-mqtt-discovery\","
    "\"name\":\"Daikin Altherma via ESPAltherma\",\"mf\":\"Daikin\","
    "\"mdl\":\"Altherma\"},"
    "\"o\":{\"name\":\"AlthermaInterface\",\"sw\":\"" FIRMWARE_VERSION "\","
    "\"url\":\"https://github.com/raomin/ESPAltherma/\"},\"cmps\":{";

static const char *kDiscoveryEnd =
    "},\"stat_t\":\"espaltherma/ATTR\",\"qos\": 2}";

// dataType values for the synthetic entries mqtt.c appends to every ATTR
// payload. They have no registry and no converter.
#define ESP_SENSOR_WIFI_RSSI 998
#define ESP_SENSOR_FREE_MEM  997
#define ESP_SENSOR_MIN_MEM   996
#define ESP_SENSOR_MAX_BLOCK 995
#define ESP_SENSOR_UPTIME    994
#define ESP_SENSOR_COMPRESSOR 993
#define ESP_SENSOR_COMPRESSOR_NUM 992
#define ESP_SENSOR_COMPRESSOR_SRC 991

// No die temperature among these: the ESP32 classic has no supported internal
// temperature sensor. The undocumented ROM temprature_sens_read() exists but is
// uncalibrated and reads a near-constant value on most chips, so publishing it
// would put an invented measurement into Home Assistant's long-term statistics.

// Lowercase, keep alphanumerics, spaces become underscores, drop the rest.
// A label made entirely of punctuation yields an EMPTY key and a unique_id of
// just "espaltherma_". Upstream's ROTEX definition has exactly such a label
// ("????"); ours renames it, and the caller skips empties regardless.
static std::string makeJsonKey(const char *input)
{
    std::string out;
    out.reserve(64);
    for (const char *p = input; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    return out;
}

static std::string getSensorDeviceAndUnit(const char *label, int convid, int dataType)
{
    switch (dataType) {
    case ESP_SENSOR_WIFI_RSSI:
        return "\"p\":\"sensor\",\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\",";
    case ESP_SENSOR_FREE_MEM:
    case ESP_SENSOR_MIN_MEM:
    case ESP_SENSOR_MAX_BLOCK:
        return "\"p\":\"sensor\",\"dev_cla\":\"data_size\",\"unit_of_meas\":\"B\",";
    case ESP_SENSOR_UPTIME:
        return "\"p\":\"sensor\",\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\",";
    case ESP_SENSOR_COMPRESSOR:
        // "ON"/"OFF" are Home Assistant's default payload_on / payload_off, so
        // this needs no payload mapping - the same as the convid 200 bits.
        return "\"p\":\"binary_sensor\",\"dev_cla\":\"running\",";
    case ESP_SENSOR_COMPRESSOR_SRC:
        // A plain text sensor, and diagnostic rather than primary: it says how
        // the compressor state was decided, not anything about the machine.
        return "\"p\":\"sensor\",";
    case ESP_SENSOR_COMPRESSOR_NUM:
        // No unit and no device class: 0/1 is a flag rendered as a number, not
        // a measured quantity in any unit. It still needs state_class
        // measurement to reach long-term statistics, which makeSensorJson()
        // adds explicitly - the usual test for a measurement requires a unit,
        // and inventing one here would be worse than special-casing it.
        return "\"p\":\"sensor\",";
    case 1:
        return "\"p\":\"sensor\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\xC2\xB0" "C\",";
    case 2:
        return "\"p\":\"sensor\",\"dev_cla\":\"pressure\",\"unit_of_meas\":\"bar\",";
    default:
        break;
    }

    // Converter-based heuristics, upstream's.
    if ((convid >= 300 && convid <= 307) || convid == 200) {
        return "\"p\":\"binary_sensor\",";
    }
    if (convid == 105 && strstr(label, "l/min") != NULL) {
        return "\"p\":\"sensor\",\"dev_cla\":\"volume_flow_rate\",\"unit_of_meas\":\"L/min\",";
    }
    if ((convid == 105 && strstr(label, "(A)") != NULL) ||
        (convid == 161 && strstr(label, "Current") != NULL)) {
        return "\"p\":\"sensor\",\"dev_cla\":\"current\",\"unit_of_meas\":\"A\",";
    }
    if (convid == 152 && strstr(label, "(rps)") != NULL) {
        return "\"p\":\"sensor\",\"dev_cla\":\"frequency\",\"unit_of_meas\":\"Hz\",";
    }
    return "\"p\":\"sensor\",";
}

// The ESP-side values are published as strings carrying their unit, so the
// template has to strip it before Home Assistant will treat them as numbers.
static std::string getConversion(int dataType)
{
    switch (dataType) {
    case ESP_SENSOR_WIFI_RSSI: return "|replace('dBm','')|int";
    case ESP_SENSOR_FREE_MEM:
    case ESP_SENSOR_MIN_MEM:
    case ESP_SENSOR_MAX_BLOCK:
    case ESP_SENSOR_UPTIME:    return "|int";
    default:                   return "";
    }
}

// A numeric converter alone does not make a value a measurement: error codes
// are numeric too, and must stay out of long-term statistics.
static bool isMeasurementSensor(const std::string &props)
{
    return props.find("\"p\":\"sensor\"") != std::string::npos &&
           props.find("\"unit_of_meas\"") != std::string::npos;
}

static std::string makeSensorJson(const char *label, int convid, int dataType,
                                  bool isDiagnostic)
{
    const std::string key = makeJsonKey(label);
    if (key.empty()) {
        return std::string();
    }
    const std::string uid = "espaltherma_" + key;

    const std::string props = getSensorDeviceAndUnit(label, convid, dataType);
    const bool isBinary = props.find("\"p\":\"binary_sensor\"") != std::string::npos;

    std::string json;
    json.reserve(320);
    json += "\"" + key + "\":{";
    json += props;
    if (isMeasurementSensor(props) || dataType == ESP_SENSOR_COMPRESSOR_NUM) {
        json += "\"stat_cla\":\"measurement\",";
    }
    json += "\"val_tpl\":\"{{value_json['";
    json += label;
    json += "']";
    json += getConversion(dataType);
    json += "}}\",";
    json += "\"uniq_id\":\"" + uid + "\",";
    json += "\"def_ent_id\":\"";
    json += isBinary ? "binary_sensor." : "sensor.";
    json += uid + "\",";
    json += "\"name\":\"";
    json += label;
    json += "\"";
    if (isDiagnostic) {
        json += ",\"ent_cat\":\"diagnostic\"";
    }
    json += "}";
    return json;
}

extern "C" const char *alt_ha_discovery_payload(size_t *len)
{
    // Held across calls so the returned pointer stays valid while esp-mqtt
    // copies it into its outbox.
    static std::string payload;

    const size_t count = converter_label_count();

    payload.clear();
    payload.reserve((count + 2) * 320 + 256);
    payload += kDiscoveryStart;

    bool first = true;
    size_t emitted = 0;
    auto append = [&](const std::string &s) {
        if (s.empty()) {
            return;
        }
        if (!first) {
            payload += ",";
        }
        payload += s;
        first = false;
        emitted++;
    };

    append(makeSensorJson("WifiRSSI", -1, ESP_SENSOR_WIFI_RSSI, true));
    append(makeSensorJson("FreeMem", -1, ESP_SENSOR_FREE_MEM, true));
    // Diagnostics that matter on a device meant to run for months: the minimum
    // free heap since boot is what reveals a slow leak, and the largest free
    // block reveals fragmentation, which a plain free-heap figure hides.
    append(makeSensorJson("MinFreeMem", -1, ESP_SENSOR_MIN_MEM, true));
    append(makeSensorJson("MaxFreeBlock", -1, ESP_SENSOR_MAX_BLOCK, true));
    append(makeSensorJson("Uptime", -1, ESP_SENSOR_UPTIME, true));

    // Not a reading and not a diagnostic: compressor state is inferred from the
    // water temperatures (derived.h), and it is one of the most useful things
    // this device can say about the machine, so it belongs with the sensors.
    append(makeSensorJson(ALT_DERIVED_COMPRESSOR_LABEL, -1, ESP_SENSOR_COMPRESSOR, false));
    // The same fact as a number, so it reaches long-term statistics: the hourly
    // mean of a 0/1 series is the compressor's duty cycle.
    append(makeSensorJson(ALT_DERIVED_COMPRESSOR_NUM_LABEL, -1,
                          ESP_SENSOR_COMPRESSOR_NUM, false));
    // Diagnostic: "power" or "delta". The two sources differ by tens of
    // seconds on every edge, so history recorded under one is not directly
    // comparable with history recorded under the other.
    append(makeSensorJson(ALT_DERIVED_COMPRESSOR_SRC_LABEL, -1,
                          ESP_SENSOR_COMPRESSOR_SRC, true));

    for (size_t i = 0; i < count; i++) {
        const char *label = NULL;
        int convid = -1;
        int dataType = -1;
        if (!converter_label_meta(i, &label, &convid, &dataType)) {
            continue;
        }
        const std::string s = makeSensorJson(label, convid, dataType, false);
        if (s.empty()) {
            ESP_LOGW(TAG, "label \"%s\" yields an empty entity key, skipped", label);
            continue;
        }
        append(s);
    }

    payload += kDiscoveryEnd;

    if (len) {
        *len = payload.size();
    }
    ESP_LOGI(TAG, "discovery payload: %u components, %u bytes",
             (unsigned)emitted, (unsigned)payload.size());
    return payload.c_str();
}
