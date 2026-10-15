#pragma once

// The bundled configurations contain Aurora's raw native structs. Compile this
// guard in the production host, including builds with BUILD_TESTING disabled:
// a renderer ABI change requires deliberate catalogue regeneration/versioning.
#include "gfx/clear.hpp"
#include "gx/pipeline.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace mscharged::runtime::pipeline_catalog_layout {

static_assert(sizeof(void*) == 8 && sizeof(bool) == 1);
static_assert(std::endian::native == std::endian::little);
static_assert(static_cast<std::uint8_t>(aurora::gfx::ShaderType::Clear) == 0);
static_assert(static_cast<std::uint8_t>(aurora::gfx::ShaderType::GX) == 1);

using Gx = aurora::gx::PipelineConfig;
static_assert(aurora::gx::GXPipelineConfigVersion == 17);
static_assert(sizeof(Gx) == 2772 && alignof(Gx) == 4);
static_assert(sizeof(aurora::gx::ShaderConfig) == 2720);
static_assert(offsetof(Gx, version) == 0);
static_assert(offsetof(Gx, msaaSamples) == 4);
static_assert(offsetof(Gx, shaderConfig) == 8);
static_assert(offsetof(Gx, depthFunc) == 2728);
static_assert(offsetof(Gx, cullMode) == 2732);
static_assert(offsetof(Gx, blendMode) == 2736);
static_assert(offsetof(Gx, blendFacSrc) == 2740);
static_assert(offsetof(Gx, blendFacDst) == 2744);
static_assert(offsetof(Gx, blendOp) == 2748);
static_assert(offsetof(Gx, dstAlpha) == 2752);
static_assert(offsetof(Gx, polygonOffsetBits) == 2756);
static_assert(offsetof(Gx, polygonOffsetScaleBits) == 2760);
static_assert(offsetof(Gx, polygonOffsetClampBits) == 2764);
static_assert(offsetof(Gx, depthCompare) == 2768);
static_assert(offsetof(Gx, depthUpdate) == 2769);
static_assert(offsetof(Gx, alphaUpdate) == 2770);
static_assert(offsetof(Gx, colorUpdate) == 2771);

using Clear = aurora::gfx::clear::PipelineConfig;
static_assert(aurora::gfx::clear::ClearPipelineConfigVersion == 5);
static_assert(sizeof(Clear) == 8 && alignof(Clear) == 4);
static_assert(offsetof(Clear, version) == 0);
static_assert(offsetof(Clear, clearColor) == 4);
static_assert(offsetof(Clear, clearAlpha) == 5);
static_assert(offsetof(Clear, clearDepth) == 6);
static_assert(offsetof(Clear, _pad) == 7);

} // namespace mscharged::runtime::pipeline_catalog_layout
