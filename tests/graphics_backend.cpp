#include "bootstrap/graphics_backend.h"

#include <iostream>

using namespace mscharged;

int main()
{
    try
    {
        auto require = [](bool value) { if (!value) throw std::runtime_error("Graphics backend selection failed"); };
#if defined(_WIN32)
        require(kGraphicsBackends.size() == 2);
        require(ResolveGraphicsBackend("vulkan").backend == GraphicsBackend::Vulkan);
        require(ResolveGraphicsBackend("d3d12").backend == GraphicsBackend::Direct3D12);
        require(ResolveGraphicsBackend("auto").backend == GraphicsBackend::Vulkan);
        const char* unsupported = "metal";
#elif defined(__APPLE__)
        require(kGraphicsBackends.size() == 1);
        require(ResolveGraphicsBackend("metal").backend == GraphicsBackend::Metal);
        require(ResolveGraphicsBackend("auto").backend == GraphicsBackend::Metal);
        const char* unsupported = "d3d12";
#else
        require(kGraphicsBackends.size() == 1);
        require(ResolveGraphicsBackend("vulkan").backend == GraphicsBackend::Vulkan);
        require(ResolveGraphicsBackend("auto").backend == GraphicsBackend::Vulkan);
        const char* unsupported = "d3d12";
#endif
        for (const char* invalid : {unsupported, "d3d11", "unknown", ""})
        {
            bool rejected = false;
            try { ResolveGraphicsBackend(invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
        }
        std::cout << "Graphics backend selection passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
