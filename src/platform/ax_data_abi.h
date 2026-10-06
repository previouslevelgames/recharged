#pragma once

#include <stdint.h>
#include <string.h>

// Source AX metadata contains native u16 fields, while MWCC accesses adjacent
// fields as a big-endian word. memcpy avoids pointer alias/alignment assumptions;
// composing the field values preserves the original high/low word semantics.
static inline uint32_t mscharged_ax_read_u16_word(const void* address) {
    uint16_t fields[2];
    memcpy(fields, address, sizeof(fields));
    return ((uint32_t)fields[0] << 16) | fields[1];
}

static inline void mscharged_ax_write_u16_word(void* address, uint32_t value) {
    const uint16_t fields[2] = {(uint16_t)(value >> 16), (uint16_t)value};
    memcpy(address, fields, sizeof(fields));
}

static inline void mscharged_ax_write_u16_word_advance(uint8_t** cursor, uint32_t value) {
    mscharged_ax_write_u16_word(*cursor, value);
    *cursor += sizeof(uint32_t);
}

// Packed source AXSTUDIO fields retain their original six-byte pair stride.
// This stores the actual native s32 metadata value, without dereferencing an
// unaligned s32*. Export to a future DSP wire format remains a separate SDK seam.
static inline void mscharged_ax_write_s32(void* address, int32_t value) {
    memcpy(address, &value, sizeof(value));
}
