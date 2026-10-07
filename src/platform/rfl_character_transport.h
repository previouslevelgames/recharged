#pragma once

#include <RVLFaceLib/RFLi_Types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exactly one original 74-byte Wii record into a separate native typed value.
 * The caller owns the immutable raw bytes and aligned output. No in-place,
 * domain guessing, cached/native input, CRC, or persistence conversion.
 * This boundary does not retain input/output pointers or allocate storage. */
void ChargedDecodeRFLCharRecordBE(const void* raw, RFLiCharData* output);

#ifdef __cplusplus
}
#endif
