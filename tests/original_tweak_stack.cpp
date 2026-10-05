#include "platform/thread.h"

#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

int IsTweakNameOnStack(const char* name);

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        char local[] = "Main stack";
        static const char persistent[] = "Static storage";
        auto heap = std::make_unique<char[]>(128);
        Check(IsTweakNameOnStack(local) == 1, "Original stack classification lost a local name");
        Check(IsTweakNameOnStack(persistent) == 0, "Original static name was unnecessarily interned");
        Check(IsTweakNameOnStack(heap.get()) == 0, "Original heap name was unnecessarily interned");
        const auto main_limits = mscharged::CurrentThreadStackLimits();
        Check(IsTweakNameOnStack(reinterpret_cast<const char*>(main_limits.low)) == 1,
              "Original inclusive lower stack boundary changed");
        Check(IsTweakNameOnStack(reinterpret_cast<const char*>(main_limits.high)) == 1,
              "Original inclusive upper stack boundary changed");
        Check(IsTweakNameOnStack(reinterpret_cast<const char*>(main_limits.low - 1)) == 0,
              "Address below actual stack accepted");
        Check(IsTweakNameOnStack(reinterpret_cast<const char*>(main_limits.high + 1)) == 0,
              "Address above actual stack accepted");

        std::promise<const char*> address;
        std::promise<void> release;
        auto release_signal = release.get_future();
        std::exception_ptr worker_error;
        std::thread worker([&]
        {
            char worker_local[] = "Worker stack";
            try
            {
                if (IsTweakNameOnStack(worker_local) != 1
                    || IsTweakNameOnStack(local) != 0
                    || IsTweakNameOnStack(persistent) != 0
                    || IsTweakNameOnStack(heap.get()) != 0)
                    throw std::runtime_error("Original classification did not query the calling thread");
                address.set_value(worker_local);
            }
            catch (...)
            {
                worker_error = std::current_exception();
                address.set_value(nullptr);
            }
            release_signal.wait();
        });
        const auto worker_address = address.get_future().get();
        const bool foreign_stack_rejected = !worker_address || IsTweakNameOnStack(worker_address) == 0;
        release.set_value();
        worker.join();
        if (worker_error) std::rethrow_exception(worker_error);
        Check(foreign_stack_rejected, "Main thread accepted a live worker's stack name");
        Check(IsTweakNameOnStack(local) == 1, "Worker queries changed the main thread's range");
        std::cout << checks << " original stack ownership checks passed, plus worker classifications\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
