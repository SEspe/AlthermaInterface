// AS number decode. Ported from upstream ESPAltherma doc/AS_NUMBER_BYTE_REFERENCE.md
// (the byte layout and the letter encoding) and from include/catalog.h (the
// lookup of candidate models). The arithmetic is the reference's, not a rewrite.

#include "as_number.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "as_table.h"

// Letter <-> the two nibbles J and K of bytes 4 and 5 (the reference's table).
// Letters I and O are not used in Daikin AS numbers.
typedef struct {
    char letter;
    uint8_t j, k;
} as_letter_t;

static const as_letter_t AS_LETTERS[] = {
    {'A', 0, 1}, {'B', 0, 2}, {'C', 0, 3}, {'D', 0, 4}, {'E', 0, 5}, {'F', 0, 6},
    {'G', 0, 7}, {'H', 0, 8}, {'J', 0, 9}, {'K', 0, 10}, {'L', 0, 11}, {'M', 0, 12},
    {'N', 0, 13}, {'P', 0, 14}, {'R', 1, 0}, {'S', 1, 1}, {'T', 1, 2}, {'U', 1, 3},
    {'V', 1, 4}, {'W', 1, 5}, {'X', 1, 6}, {'Y', 1, 7}, {'Z', 1, 8},
};
#define AS_LETTER_COUNT (sizeof(AS_LETTERS) / sizeof(AS_LETTERS[0]))

bool alt_as_parse(const char *text, alt_as_number_t *out)
{
    const char *p = text;
    alt_as_number_t as = {0};

    while (*p == ' ') {
        p++;
    }
    if ((p[0] == 'A' || p[0] == 'a') && (p[1] == 'S' || p[1] == 's')) {
        p += 2;
    }

    for (int i = 0; i < 7; i++) {
        if (!isdigit((unsigned char)p[i])) {
            return false;
        }
        as.digits[i] = (uint8_t)(p[i] - '0');
    }
    p += 7;

    // Separator before the suffix: a dash, a space, or both.
    while (*p == ' ' || *p == '-') {
        p++;
    }
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) {
        return false;
    }
    as.s1 = (uint8_t)(p[0] - '0');
    as.s2 = (uint8_t)(p[1] - '0');
    p += 2;

    while (*p == ' ') {
        p++;
    }
    if (*p != '\0') {
        char c = (char)toupper((unsigned char)*p);
        bool known = false;
        for (size_t i = 0; i < AS_LETTER_COUNT; i++) {
            if (AS_LETTERS[i].letter == c) {
                known = true;
            }
        }
        if (!known) {
            return false;
        }
        as.letter = c;
        p++;
        while (*p == ' ') {
            p++;
        }
        if (*p != '\0') {
            return false;
        }
    }

    *out = as;
    return true;
}

void alt_as_format(const alt_as_number_t *as, char *out, size_t len)
{
    char d[8];
    for (int i = 0; i < 7; i++) {
        d[i] = (char)('0' + as->digits[i]);
    }
    d[7] = '\0';
    if (as->letter) {
        snprintf(out, len, "AS%s-%u%u%c", d, as->s1, as->s2, as->letter);
    } else {
        snprintf(out, len, "AS%s-%u%u", d, as->s1, as->s2);
    }
}

void alt_as_to_bytes(const alt_as_number_t *as, uint8_t out[6])
{
    const uint8_t *d = as->digits; // d[0] is d1 ... d[6] is d7
    uint8_t j = 0, k = 0;
    for (size_t i = 0; i < AS_LETTER_COUNT; i++) {
        if (as->letter && AS_LETTERS[i].letter == as->letter) {
            j = AS_LETTERS[i].j;
            k = AS_LETTERS[i].k;
        }
    }

    out[0] = (uint8_t)(d[0] << 4);                    // d1
    out[1] = (uint8_t)((d[3] << 4) | d[2]);           // d4, d3
    out[2] = (uint8_t)((d[4] << 4) | d[1]);           // d5, d2
    out[3] = (uint8_t)((d[6] << 4) | d[5]);           // d7, d6
    out[4] = (uint8_t)((j << 4) | as->s2);            // J, s2
    out[5] = (uint8_t)((k << 4) | as->s1);            // K, s1
}

void alt_as_from_bytes(const uint8_t in[6], alt_as_number_t *out)
{
    alt_as_number_t as = {0};
    as.digits[0] = in[0] >> 4;   // d1
    as.digits[1] = in[2] & 0x0F; // d2
    as.digits[2] = in[1] & 0x0F; // d3
    as.digits[3] = in[1] >> 4;   // d4
    as.digits[4] = in[2] >> 4;   // d5
    as.digits[5] = in[3] & 0x0F; // d6
    as.digits[6] = in[3] >> 4;   // d7
    as.s2 = in[4] & 0x0F;
    as.s1 = in[5] & 0x0F;

    uint8_t j = in[4] >> 4, k = in[5] >> 4;
    for (size_t i = 0; i < AS_LETTER_COUNT; i++) {
        if (AS_LETTERS[i].j == j && AS_LETTERS[i].k == k) {
            as.letter = AS_LETTERS[i].letter;
        }
    }
    *out = as;
}

size_t alt_as_models(const alt_as_number_t *as, const char **names, size_t max)
{
    uint32_t body = 0;
    for (int i = 0; i < 7; i++) {
        body = body * 10 + as->digits[i];
    }

    // Every table row for this body whose suffix digits agree (0xFF: any).
    uint64_t mask = 0;
    for (const as_entry_t *e = AS_ENTRIES; e->body != 0; e++) {
        if (e->body != body) {
            continue;
        }
        if (e->suffix1 != 0xFF && e->suffix1 != as->s1) {
            continue;
        }
        if (e->suffix2 != 0xFF && e->suffix2 != as->s2) {
            continue;
        }
        mask |= e->models;
    }

    size_t count = 0;
    for (size_t i = 0; i < AS_MODEL_COUNT; i++) {
        if (!((mask >> i) & 1)) {
            continue;
        }
        if (count < max) {
            names[count] = AS_MODEL_NAMES[i];
        }
        count++;
    }
    return count;
}
