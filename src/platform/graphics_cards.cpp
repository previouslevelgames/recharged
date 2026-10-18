#include "platform/graphics_cards.h"

#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>
#elif defined(MSCHARGED_GRAPHICS_CARDS_VULKAN)
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#endif

namespace mscharged::platform
{
namespace
{
[[maybe_unused]] void Add(std::vector<GraphicsCard>& cards, std::uint32_t vendor, std::uint32_t device, std::string name)
{
    // PCI IDs only: larger vendor IDs are Khronos ones of software and embedded drivers.
    if (vendor == 0 || vendor > 0xFFFF || device > 0xFFFF) return;
    for (const auto& card : cards)
        if (card.id == GraphicsCardId{vendor, device}) return;
    cards.push_back({{vendor, device}, std::move(name)});
}

#if defined(_WIN32)
std::string Utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(std::size_t(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}
#endif
} // namespace

std::vector<GraphicsCard> ListGraphicsCards()
{
    std::vector<GraphicsCard> cards;
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return cards;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            Add(cards, description.VendorId, description.DeviceId, Utf8(description.Description));
        adapter->Release();
    }
    factory->Release();
#elif defined(MSCHARGED_GRAPHICS_CARDS_VULKAN)
    // The loader stays loaded: drivers do not all like being unloaded, and the
    // game opens it again anyway.
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) return cards;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!get) return cards;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Recharged launcher";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (!create || create(&info, nullptr, &instance) != VK_SUCCESS) return cards;
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    const auto enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
    const auto properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
    std::uint32_t count = 0;
    if (enumerate && properties && enumerate(instance, &count, nullptr) == VK_SUCCESS && count)
    {
        std::vector<VkPhysicalDevice> devices(count);
        if (enumerate(instance, &count, devices.data()) >= VK_SUCCESS)
            for (std::uint32_t i = 0; i < count; ++i)
            {
                VkPhysicalDeviceProperties device{};
                properties(devices[i], &device);
                if (device.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
                    Add(cards, device.vendorID, device.deviceID, device.deviceName);
            }
    }
    if (destroy) destroy(instance, nullptr);
#endif
    return cards;
}

std::string DescribeGraphicsCards(const std::vector<GraphicsCard>& cards)
{
    std::string line = "Graphics cards:";
    for (std::size_t i = 0; i < cards.size(); ++i)
        line += std::string(i ? ", " : " ") + cards[i].name + " (" + GraphicsCardSetting(cards[i].id) + ")";
    return cards.empty() ? line + " none listed" : line;
}
} // namespace mscharged::platform
