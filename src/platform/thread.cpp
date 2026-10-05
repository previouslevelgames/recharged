#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "platform/thread.h"

#include <limits>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace mscharged
{
ThreadStackLimits CurrentThreadStackLimits()
{
    std::uintptr_t low = 0, high = 0;
#if defined(_WIN32)
    ULONG_PTR lower = 0, upper = 0;
    GetCurrentThreadStackLimits(&lower, &upper);
    low = lower;
    high = upper;
#elif defined(__APPLE__)
    const auto current = pthread_self();
    high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(current));
    const auto size = pthread_get_stacksize_np(current);
    if (size > high) throw std::runtime_error("Invalid native thread stack size");
    low = high - size;
#elif defined(__linux__)
    pthread_attr_t attributes;
    const int attribute_error = pthread_getattr_np(pthread_self(), &attributes);
    if (attribute_error)
        throw std::system_error(attribute_error, std::generic_category(), "Read current thread attributes");
    void* address = nullptr;
    std::size_t size = 0;
    const int stack_error = pthread_attr_getstack(&attributes, &address, &size);
    const int destroy_error = pthread_attr_destroy(&attributes);
    if (stack_error)
        throw std::system_error(stack_error, std::generic_category(), "Read current thread stack");
    if (destroy_error)
        throw std::system_error(destroy_error, std::generic_category(), "Destroy current thread attributes");
    low = reinterpret_cast<std::uintptr_t>(address);
    if (size > std::numeric_limits<std::uintptr_t>::max() - low)
        throw std::runtime_error("Native thread stack address overflow");
    high = low + size;
#else
    throw std::runtime_error("Current thread stack limits are unavailable on this platform");
#endif
    if (!low || high <= low) throw std::runtime_error("Invalid native thread stack limits");
    return {low, high};
}
}
