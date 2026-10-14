#pragma once

#include <memory>
#include <optional>

namespace mscharged::platform {

struct SystemUsage {
    std::optional<double> cpu_percent;
    std::optional<double> gpu_percent;
};

// Total system load, including other applications. GPU is the busiest reported
// device/engine, never a sum of engines that can execute concurrently. Missing
// counters stay unavailable. Sampling runs every 500 ms on a private worker;
// the presentation thread only copies the latest result. Create only when used.
class SystemUsageMonitor {
public:
    SystemUsageMonitor();
    ~SystemUsageMonitor();
    SystemUsageMonitor(const SystemUsageMonitor&) = delete;
    SystemUsageMonitor& operator=(const SystemUsageMonitor&) = delete;
    SystemUsage Latest() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mscharged::platform
