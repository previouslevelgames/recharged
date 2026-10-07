#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Actual completed serialized bytes are mandatory; unknown MEM temps reject. */
uint16_t ChargedReadRFLShapeHalf(const void* word);
int16_t ChargedReadRFLShapeS16(const void* word);
void ChargedCopyRFLShapeVector(void* destination, const void* source);
void ChargedCopyRFLShapeBytes(void* destination, const void* source, size_t bytes);
void ChargedTransformRFLShapeCoordinate(int16_t* destination, const int16_t* source);

/* Resolution retains the actual original attribute/address/stride request. */
void ChargedSetRFLGraphicsArray(int attribute, const void* data, uint8_t stride);

#ifdef __cplusplus
}
#endif
