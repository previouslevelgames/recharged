#pragma once
#include "resources/static_model.h"
#include "resources/texture_bundle.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"

namespace mscharged
{
// A single pool marker owns this static batch. Fixed-width disc records have
// already been validated; this adapter constructs native game records without
// casting the original binary layouts onto host pointers.
class StaticInventory
{
    GLResourcePool& pool_;
    GLResourceMark mark_ = 0;
    void (*before_release_)() = nullptr;
public:
    StaticInventory(GLResourcePool& pool, const std::vector<resources::StaticModel>& models,
        const std::vector<resources::Texture>& textures, void (*before_release)() = nullptr);
    ~StaticInventory();
    StaticInventory(const StaticInventory&) = delete;
    StaticInventory& operator=(const StaticInventory&) = delete;
    glModel* Model(std::uint32_t id) const;
    void Release();
};
}
