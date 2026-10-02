#pragma once
#include "resources/static_model.h"
#include "NL/gl/glModel.h"
#include <memory>

namespace mscharged
{
// Selected original program instances own registry nodes until explicit teardown.
class MaterialPrograms
{
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    MaterialPrograms();
    ~MaterialPrograms();
    void Release();
    MaterialPrograms(const MaterialPrograms &) = delete;
    MaterialPrograms &operator=(const MaterialPrograms &) = delete;
};
std::size_t MaterialParameterSize(std::uint32_t program);
void InstallMaterial(glModelPacket &packet, const resources::Material &material, void *storage);
std::vector<std::uint32_t> MaterialLookupTextures(const resources::StaticModel &model);
void DrawMaterial(glModelPacket &packet);
} // namespace mscharged
