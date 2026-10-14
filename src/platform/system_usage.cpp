#include "platform/system_usage.h"
#include "platform/system_usage_detail.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mscharged::platform {
namespace {
using detail::UsagePercent;

[[maybe_unused]] void Include(std::optional<double>& busiest, std::optional<double> value)
{
    if (value) busiest = std::max(busiest.value_or(0), *value);
}

#if defined(_WIN32)
class UsageReader {
public:
    UsageReader()
    {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        // Processor Information covers all processor groups, including >64 CPUs.
        if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Time", 0, &cpu_)
            != ERROR_SUCCESS) cpu_ = nullptr;
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_)
            != ERROR_SUCCESS) gpu_ = nullptr;
    }
    ~UsageReader() { if (query_) PdhCloseQuery(query_); }

    SystemUsage Read()
    {
        SystemUsage result;
        // Also works when performance counters are disabled (and under Wine).
        // GetSystemTimes covers only one group on >64-CPU systems, so use PDH
        // alone there rather than labelling a partial group as total usage.
        std::optional<detail::CpuTimes> times;
        FILETIME idle{}, kernel{}, user{};
        if (GetActiveProcessorGroupCount() == 1 && GetSystemTimes(&idle, &kernel, &user)) {
            const auto total = Ticks(kernel) + Ticks(user); // kernel includes idle
            if (total >= Ticks(idle)) times = detail::CpuTimes{total - Ticks(idle), total};
        }
        result.cpu_percent = cpuFallback_.Sample(times);
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return result;
        PDH_FMT_COUNTERVALUE value{};
        if (cpu_ && PdhGetFormattedCounterValue(cpu_, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS
            && Valid(value.CStatus)) result.cpu_percent = UsagePercent(value.doubleValue);
        DWORD bytes = 0, count = 0;
        if (!gpu_ || PdhGetFormattedCounterArrayW(gpu_, PDH_FMT_DOUBLE, &bytes, &count, nullptr)
            != static_cast<PDH_STATUS>(PDH_MORE_DATA))
            return result;
        // Aligned storage; a hotplug/instance change can invalidate this sample.
        std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer(
            (bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1) / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        if (PdhGetFormattedCounterArrayW(gpu_, PDH_FMT_DOUBLE, &bytes, &count, buffer.data()) != ERROR_SUCCESS)
            return result;
        detail::GpuEngineUsage engines;
        for (DWORD n = 0; n < count; ++n)
            if (buffer[n].szName && Valid(buffer[n].FmtValue.CStatus))
                engines.Add(buffer[n].szName, buffer[n].FmtValue.doubleValue);
        result.gpu_percent = engines.Busiest();
        return result;
    }
private:
    static std::uint64_t Ticks(FILETIME value)
    {
        return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    }
    static bool Valid(DWORD status) { return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA; }
    detail::UsageDelta cpuFallback_;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER cpu_ = nullptr, gpu_ = nullptr;
};
#elif defined(__APPLE__)
class UsageReader {
public:
    ~UsageReader() { mach_port_deallocate(mach_task_self(), host_); }
    SystemUsage Read()
    {
        SystemUsage result;
        host_cpu_load_info_data_t load{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        std::optional<detail::CpuTimes> times;
        if (host_statistics(host_, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&load), &count) == KERN_SUCCESS) {
            const auto busy = std::uint64_t(load.cpu_ticks[CPU_STATE_USER]) + load.cpu_ticks[CPU_STATE_SYSTEM]
                + load.cpu_ticks[CPU_STATE_NICE];
            times = detail::CpuTimes{busy, busy + load.cpu_ticks[CPU_STATE_IDLE]};
        }
        result.cpu_percent = cpu_.Sample(times);
        io_iterator_t iterator = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &iterator) != KERN_SUCCESS)
            return result;
        while (const auto service = IOIteratorNext(iterator)) {
            const auto stats = IORegistryEntryCreateCFProperty(service, CFSTR("PerformanceStatistics"),
                                                               kCFAllocatorDefault, 0);
            // This optional driver property is not present on every Mac GPU.
            if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
                const auto value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(stats), CFSTR("Device Utilization %"));
                double percent = 0;
                if (value && CFGetTypeID(value) == CFNumberGetTypeID()
                    && CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &percent))
                    Include(result.gpu_percent, UsagePercent(percent));
            }
            if (stats) CFRelease(stats);
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        return result;
    }
private:
    mach_port_t host_ = mach_host_self();
    detail::UsageDelta cpu_;
};
#elif defined(__linux__)
// NVML ships with the NVIDIA driver. Load its stable C API only when present;
// neither a vendor SDK nor a subprocess is required by the port.
class NvidiaUsage {
public:
    NvidiaUsage()
    {
        library_ = dlopen("libnvidia-ml.so.1", RTLD_LOCAL | RTLD_LAZY);
        if (!library_) return;
        const auto init = reinterpret_cast<int(*)()>(dlsym(library_, "nvmlInit_v2"));
        shutdown_ = reinterpret_cast<int(*)()>(dlsym(library_, "nvmlShutdown"));
        const auto count = reinterpret_cast<int(*)(unsigned*)>(dlsym(library_, "nvmlDeviceGetCount_v2"));
        const auto device = reinterpret_cast<int(*)(unsigned, void**)>(dlsym(library_, "nvmlDeviceGetHandleByIndex_v2"));
        utilization_ = reinterpret_cast<int(*)(void*, Utilization*)>(dlsym(library_, "nvmlDeviceGetUtilizationRates"));
        if (!init || !shutdown_ || !count || !device || !utilization_ || init() != 0) return;
        initialized_ = true;
        unsigned size = 0;
        if (count(&size) != 0) return;
        for (unsigned n = 0; n < size; ++n) {
            void* handle = nullptr;
            if (device(n, &handle) == 0) devices_.push_back(handle);
        }
    }
    ~NvidiaUsage()
    {
        if (initialized_) shutdown_();
        if (library_) dlclose(library_);
    }
    std::optional<double> Read() const
    {
        std::optional<double> result;
        for (auto* device : devices_) {
            Utilization utilization{};
            if (utilization_(device, &utilization) == 0) Include(result, UsagePercent(utilization.gpu));
        }
        return result;
    }
private:
    struct Utilization { unsigned gpu, memory; };
    void* library_ = nullptr;
    int (*shutdown_)() = nullptr;
    int (*utilization_)(void*, Utilization*) = nullptr;
    bool initialized_ = false;
    std::vector<void*> devices_;
};

class IntelUsage {
public:
    IntelUsage()
    {
        std::error_code error;
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator pmus("/sys/bus/event_source/devices", error);
             !error && pmus != end; pmus.increment(error)) {
            const auto name = pmus->path().filename().string();
            if (name != "i915" && name.compare(0, 5, "i915_") != 0) continue;
            unsigned type;
            if (!(std::ifstream(pmus->path() / "type") >> type)) continue;
            int cpu = 0;
            std::ifstream(pmus->path() / "cpumask") >> cpu;
            std::error_code eventError;
            for (std::filesystem::directory_iterator events(pmus->path() / "events", eventError);
                 !eventError && events != end; events.increment(eventError)) {
                const auto event = events->path().filename().string();
                if (event.size() < 5 || event.compare(event.size() - 5, 5, "-busy") != 0) continue;
                std::string config, unit;
                std::ifstream(events->path()) >> config;
                std::ifstream(events->path().string() + ".unit") >> unit;
                if (config.compare(0, 9, "config=0x") != 0 || unit != "ns") continue;
                char* last = nullptr;
                const auto value = std::strtoull(config.c_str() + 9, &last, 16);
                if (!last || *last) continue;
                perf_event_attr attr{};
                attr.size = sizeof attr;
                attr.type = type;
                attr.config = value;
                attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED;
                const int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC));
                // No elevation or permission changes; N/A when access is denied.
                if (fd >= 0) engines_.push_back({fd, {}});
            }
        }
    }
    ~IntelUsage() { for (const auto& engine : engines_) close(engine.fd); }
    std::optional<double> Read()
    {
        std::optional<double> result;
        for (auto& engine : engines_) {
            std::uint64_t values[2]{}; // engine busy ns, enabled ns
            std::optional<detail::CpuTimes> times;
            if (read(engine.fd, values, sizeof values) == sizeof values) times = {values[0], values[1]};
            Include(result, engine.usage.Sample(times));
        }
        return result;
    }
private:
    struct Engine { int fd; detail::UsageDelta usage; };
    std::vector<Engine> engines_;
};

class UsageReader {
public:
    UsageReader()
    {
        std::error_code error;
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator cards("/sys/class/drm", error);
             !error && cards != end; cards.increment(error)) {
            const auto name = cards->path().filename().string();
            if (name.compare(0, 4, "card") != 0 || name.size() <= 4
                || name.find_first_not_of("0123456789", 4) != std::string::npos) continue;
            const auto busy = cards->path() / "device/gpu_busy_percent";
            if (std::filesystem::exists(busy, error)) busyFiles_.push_back(busy);
        }
    }
    SystemUsage Read()
    {
        std::ifstream stat("/proc/stat");
        SystemUsage result{cpu_.Sample(detail::ReadProcCpuTimes(stat)), nvidia_.Read()};
        Include(result.gpu_percent, intel_.Read());
        for (const auto& path : busyFiles_) {
            double busy;
            if (std::ifstream(path) >> busy) Include(result.gpu_percent, UsagePercent(busy));
        }
        return result;
    }
private:
    detail::UsageDelta cpu_;
    NvidiaUsage nvidia_;
    IntelUsage intel_;
    std::vector<std::filesystem::path> busyFiles_;
};
#else
struct UsageReader { SystemUsage Read() { return {}; } };
#endif
} // namespace

struct SystemUsageMonitor::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    SystemUsage latest;
    std::thread worker;

    Impl() : worker([this] { Run(); }) {}
    ~Impl()
    {
        { std::lock_guard lock(mutex); stop = true; }
        wake.notify_one();
        worker.join();
    }
    void Run() noexcept
    {
        try {
            UsageReader reader;
            for (;;) {
                const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                const auto sample = reader.Read();
                std::unique_lock lock(mutex);
                latest = sample;
                if (wake.wait_until(lock, next, [this] { return stop; })) break;
            }
        } catch (...) {
            // Optional telemetry must never interrupt the game or retain stale values.
            std::lock_guard lock(mutex);
            latest = {};
        }
    }
};

SystemUsageMonitor::SystemUsageMonitor() : impl_(std::make_unique<Impl>()) {}
SystemUsageMonitor::~SystemUsageMonitor() = default;
SystemUsage SystemUsageMonitor::Latest() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->latest;
}

} // namespace mscharged::platform
