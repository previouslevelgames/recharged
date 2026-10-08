#pragma once

namespace mscharged::platform
{
// Typed bytes beneath the original Layout animation loader. The caller owns
// the real source ARC lifetime. No allocation/animation/UI readiness is granted.
// Supported nested records: original RLAN0.8, fileNum0, Hermite RLPA/RLVC/RLMC.
// Header-only rejected BOM/version inputs preserve the original predicate.
const void* NativeHBMAnimationHeader(const void* originalResource);
}
