#pragma once

// Registry bytes -> labelled values. Thin C facade over the C++ Converter
// ported from upstream ESPAltherma's include/converters.h (MIT, (c) 2020
// Raomin). The label table itself comes from the definition file selected in
// model_config.h, which is C++ (LabelDef has constructors), so it stays behind
// this wall and main.c never sees it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 'I' or 'S', from the selected definition file.
char converter_protocol(void);

// Refrigerant used by the pressure->temperature conversions (convid 401-406):
// 801 = R410A, 802 = R32, 803 = R22. A definition file carrying convid 800-803
// overrides this at runtime, exactly as upstream does.
void converter_set_refrigerant(int rtype);
int  converter_refrigerant(void);

// Distinct registry IDs named by the definition file, in first-seen order.
// Returns how many were written to out.
size_t converter_registry_ids(uint8_t *out, size_t max);

// Decodes one raw reply in place, updating the stored value of every label
// belonging to that registry. `data` is the full reply buffer as received.
void converter_read_registry(const uint8_t *data, char protocol);

// Total labels in the selected definition file.
size_t converter_label_count(void);

// Reads back label i. `label` and `value` point into the converter's own
// storage and stay valid until the next converter_read_registry() for that
// label's registry. Returns false if i is out of range.
bool converter_label_at(size_t i, uint8_t *reg_id, const char **label, const char **value);

// Static metadata for label i: its name, conversion id and dataType hint.
// Home Assistant discovery needs these to pick a device class, and routing it
// through here keeps labelDefs[] defined in exactly one translation unit - the
// definition headers declare the array, so including one from two .cpp files
// is a duplicate-symbol link error.
bool converter_label_meta(size_t i, const char **label, int *convid, int *data_type);

#ifdef __cplusplus
}
#endif
