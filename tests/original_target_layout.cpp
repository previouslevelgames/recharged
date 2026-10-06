#include "NL/gl/glTarget.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#if defined(MSCHARGED_DIAGNOSTIC_VIEWS)
#error Source target layout gate cannot use legacy view instrumentation
#endif
static_assert(sizeof(GLTargetInfo) == 40);
static_assert(offsetof(GLTargetInfo, unknown08) == 8);
static_assert(offsetof(GLTargetInfo, unknown0C) == 12);
static_assert(sizeof(GLRenderPair) == 16);
static_assert(offsetof(GLRenderPair, target) == 8);

int main()
{
    // Only initialized source members are observable. The source projection
    // proves its exact constructor body separately; padding and uninitialized
    // members cannot be used as a Release optimizer oracle after placement new.
    GLTargetInfo info;
    if (info.unknown08 || info.unknown0C)
        throw std::runtime_error("Original target initialized members changed");
    const auto* pointer = reinterpret_cast<GLXTarget*>(std::uintptr_t(0x123456789ULL));
    GLRenderPair pair(0xFEDCBA98u, const_cast<GLXTarget*>(pointer));
    if (pair.hash != 0xFEDCBA98u || pair.target != pointer || !pair)
        throw std::runtime_error("Original target handle store or native pointer changed");
    GLRenderPair empty;
    if (empty.hash || empty.target || empty)
        throw std::runtime_error("Original default target handle changed");
    pair.hash = 0;
    if (pair)
        throw std::runtime_error("Original target boolean requires both fields");
    pair.hash = 1;
    pair.target = nullptr;
    if (pair)
        throw std::runtime_error("Original target boolean requires real pointer");
    std::puts("Original target constructor/native handle: 5 checks; no target service/GPU acceptance");
}
