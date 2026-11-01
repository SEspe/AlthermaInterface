#pragma once

// AS number of the indoor unit: parsing, the six-byte layout of register 0x63
// and the model candidates it points to. Upstream ESPAltherma reads the AS number
// from 0x63 on protocol I machines; on this unit (protocol S, EKHBH008BA) that
// register does not answer, so the AS number is typed in on the Config tab and
// read from the indoor unit's label. Layout: doc/AS_NUMBER_BYTE_REFERENCE.md
// upstream, table: as_table.h (copied from upstream catalog_data.h).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t digits[7]; // d1..d7 of "AS d1 d2 d3 d4 d5 d6 d7"
    uint8_t s1, s2;    // the two suffix digits of "- s1 s2 L"
    char letter;       // the trailing letter, 0 when there is none
} alt_as_number_t;

// Reads "AS1709217-30A". Spaces and a missing "AS" prefix are tolerated, so a
// label copied as "AS1709217-30 A" works. False when the text is not an AS
// number (the letters I and O are not used by Daikin).
bool alt_as_parse(const char *text, alt_as_number_t *out);

// Writes "AS1709217-30A" (the letter is omitted when there is none).
void alt_as_format(const alt_as_number_t *as, char *out, size_t len);

// Register 0x63, offsets 2..7: the six bytes the unit reports for this number.
void alt_as_to_bytes(const alt_as_number_t *as, uint8_t out[6]);

// The inverse of alt_as_to_bytes().
void alt_as_from_bytes(const uint8_t in[6], alt_as_number_t *out);

// Names of the definition files this AS number points to, in table order, at
// most max of them. Returns how many there are (which may exceed max).
size_t alt_as_models(const alt_as_number_t *as, const char **names, size_t max);
