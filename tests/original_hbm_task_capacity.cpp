#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include "revolution/os/OSThread.h"
#include "revolution/os/OSContext.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <stdexcept>

namespace {
template<class F> F Symbol(void* module, const char* name) {
    dlerror();
    auto* address = dlsym(module, name);
    const auto* error = dlerror();
    if (error || !address) throw std::runtime_error(error ? error : "Missing source task capacity export");
    return reinterpret_cast<F>(address);
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected the original shared HBM task module");
        auto* caller = OSGetCurrentThread();
        auto* context = OSGetCurrentContext();
        auto* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!module) throw std::runtime_error(dlerror());
        const auto capacity = Symbol<unsigned(*)()>(module, "charged_hbm_task_capacity");
        if (!capacity()) throw std::runtime_error("Source capacity qualifier performed no checks");
        if (OSGetCurrentThread() != caller || OSGetCurrentContext() != context
            || !mscharged::platform::NativeInterruptsEnabled())
            throw std::runtime_error("Original task methods changed the caller owner/context/mask");
        mscharged::platform::DrainNativeThreadLifetimes();
        if (dlclose(module)) throw std::runtime_error("Quiescent task module did not unload");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Original HBM task capacity failed: %s\n", error.what());
        std::fflush(nullptr);
        std::_Exit(1); // Preserve any failed source owner; no cancellation substitute.
    }
}
