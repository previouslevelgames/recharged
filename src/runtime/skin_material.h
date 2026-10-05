#pragma once
#include "resources/skin_model.h"
#include "NL/gl/glModel.h"
namespace mscharged
{
// The selected original material keeps its TEV/light/alpha decisions. Native
// adaptation changes packet storage and GX command transport only.
void ValidateNativeSkinPacket(const glModelPacket&);
void DrawNativeSkinPacket(const glModelPacket&);
void SkinNormalMatrix(const float (&input)[3][4],float (&output)[3][4]);
}
