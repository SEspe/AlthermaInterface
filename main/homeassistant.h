#pragma once

// Home Assistant MQTT discovery. Ported from upstream ESPAltherma's
// src/homeassistant.cpp + include/homeassistant.h (MIT, (c) 2020 Raomin).
//
// Publishes one retained device-discovery payload describing every label in the
// active definition file, so the entities appear in Home Assistant without any
// YAML. The device identifiers and the state topic are kept identical to
// upstream so an existing setup, or a second unit still running upstream's
// firmware, needs no reconfiguration.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALT_HA_DISCOVERY_TOPIC "homeassistant/device/espaltherma-mqtt-discovery/config"

// Builds the discovery payload for the active definition file. Returns a
// NUL-terminated buffer owned by this module, valid until the next call, and
// writes its length to *len. Returns NULL if it could not be built.
const char *alt_ha_discovery_payload(size_t *len);

#ifdef __cplusplus
}
#endif
