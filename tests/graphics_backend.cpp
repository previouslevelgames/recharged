#include "bootstrap/graphics_backend.h"
#include "bootstrap/graphics_card.h"

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
        // auto alone may try DirectX 12 after Vulkan; a chosen API never switches.
        require(GraphicsBackendFallback("auto") && GraphicsBackendFallback("auto")->backend == GraphicsBackend::Direct3D12);
        require(!GraphicsBackendFallback("vulkan") && !GraphicsBackendFallback("d3d12"));
        require(GraphicsStartFailure(ResolveGraphicsBackend("auto"), GraphicsBackendFallback("auto"))
            == "Neither Vulkan nor DirectX 12 could start on this PC. Updating the graphics driver may help.");
        require(GraphicsStartFailure(ResolveGraphicsBackend("d3d12"), nullptr)
            == "DirectX 12 could not start on this PC. Choose another graphics API on the launcher's Advanced page, "
               "or update the graphics driver.");
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
#if !defined(_WIN32)
        // One API: nothing to fall back to, and no other choice to offer.
        require(!GraphicsBackendFallback("auto"));
        require(GraphicsStartFailure(ResolveGraphicsBackend("auto"), nullptr)
            == std::string(kGraphicsBackends.front().label) + " could not start on this PC. Updating the graphics driver may help.");
#endif
        // display.gpu: auto or a card's PCI ID, saved in lower case.
        require(!ParseGraphicsCard("auto"));
        require(ParseGraphicsCard("10de:1f99") == GraphicsCardId{0x10de, 0x1f99});
        require(ParseGraphicsCard("8086:3EA0") == GraphicsCardId{0x8086, 0x3ea0});
        require(GraphicsCardSetting({0x10de, 0x1f99}) == "10de:1f99" && GraphicsCardSetting({0x1002, 0x73df}) == "1002:73df");
        for (const char* invalid : {"", "Auto", "nvidia", "10de-1f99", "10de:1f9", "10de:1f99 ", "0000:1f99", "10dex1f99",
                                    "g0de:1f99"})
        {
            bool rejected = false;
            try { ParseGraphicsCard(invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
        }
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
