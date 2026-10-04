#include "runtime/boot_loading.h"
#include "runtime/boot_script_load.h"
#include "runtime/startup.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTextureManager.h"
#include <chrono>
#include <thread>

namespace mscharged
{
namespace
{
struct DiagnosticMemory
{
    DiagnosticMemory()
    {
        if (glGetResourcePools() || glGetTextureManager())
            throw std::logic_error("Boot diagnostic requires its own graphics memory lifetime");
        glInitResourcePools();
        try { InitializeOriginalGraphicsMemory(); }
        catch (...) { glShutdownMemory(); throw; }
    }
    ~DiagnosticMemory() { glShutdownMemory(); }
};
}
std::string VerifyStartupBootLoading()
{
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    std::string result;
    {
        BootScriptLoad load;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (load.State() == BootScriptLoadState::Loading)
        {
            load.Service();
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Boot script diagnostic timed out while reading");
            if (load.State() == BootScriptLoadState::Loading)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto asset = load.Result();
        DiagnosticMemory memory; // Selected original memory callback, not full glStartup.
        BootLoading boot(asset->bytes);
        boot.Begin();
        while (boot.Update() == BootLoadingState::Running)
        {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Boot script diagnostic exceeded its execution deadline");
        }
        if (boot.State() == BootLoadingState::Failed)
            throw std::runtime_error("Original boot script execution failed: " + boot.Error());
        result = "Independent original BootLoadingToFE diagnostic: " + std::to_string(asset->bytes.size())
            + " script bytes, " + std::to_string(boot.HostCalls()) + " host calls; ";
        if (boot.State() == BootLoadingState::Blocked)
        {
            const auto stop = boot.Stop();
            if (!stop) throw std::logic_error("Blocked boot script has no service diagnosis");
            result += "blocked at service " + std::to_string(stop->service) + " (" + stop->description + ").";
            if (stop->service == 41) result += " " + VerifyStartupParticleResources();
        }
        else if (boot.State() == BootLoadingState::Complete)
            result += "selected script finished; full frontend initialization remains separate.";
        else throw std::logic_error("Boot script reached an unexpected terminal state");
    }
    if (StandardAllocator.TotalFreeMemory() != mem1 || VirtualAllocator.TotalFreeMemory() != mem2)
        throw std::runtime_error("Boot diagnostic did not recover both game arenas");
    return result + " Both arenas recovered; remaining Initialize stages are still unlinked.";
}
}
