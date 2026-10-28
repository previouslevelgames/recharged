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
/* Texcoord s16 pairs, converted to native order like the positions and
   normals: RFLiInitCharModel publishes the model's arrays as native bytes. */
void ChargedCopyRFLShapeTexcoords(void* destination, const void* source, size_t bytes);
void ChargedTransformRFLShapeCoordinate(int16_t* destination, const int16_t* source);

/* Resolution retains the actual original attribute/address/stride request. */
void ChargedSetRFLGraphicsArray(int attribute, const void* data, uint8_t stride);

/* RFLiInitCharModel writes a model's vertex arrays natively into its
   RFLiCharModelRes: that struct becomes graphics storage of native bytes, so
   RFLDrawOpa/Xlu's GXSetArray resolves into it. A model built again at the
   same address (a freed and reused work buffer) replaces the storage. */
void ChargedPublishRFLModelArrays(void* res, size_t bytes);

#ifdef __cplusplus
}
#endif
