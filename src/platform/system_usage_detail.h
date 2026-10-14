#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mscharged::platform::detail {

inline std::optional<double> UsagePercent(double value)
{
    if (!std::isfinite(value) || value < 0) return std::nullopt;
    return std::min(value, 100.0);
}

struct CpuTimes { std::uint64_t busy, total; };

// /proc/stat's guest times are already included in user/nice. I/O wait is
// idle CPU time; excluding it from busy exposes actual CPU contention.
inline std::optional<CpuTimes> ReadProcCpuTimes(std::istream& input)
{
    std::string name;
    std::uint64_t user, nice, system, idle, wait, irq, softirq, steal;
    if (!(input >> name >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal) || name != "cpu")
        return std::nullopt;
    const auto busy = user + nice + system + irq + softirq + steal;
    return CpuTimes{busy, busy + idle + wait};
}

class UsageDelta {
public:
    std::optional<double> Sample(std::optional<CpuTimes> current)
    {
        const auto previous = previous_;
        previous_ = current;
        if (!current || !previous || current->total <= previous->total || current->busy < previous->busy)
            return std::nullopt;
        const auto total = current->total - previous->total;
        const auto busy = current->busy - previous->busy;
        if (busy > total) return std::nullopt;
        return 100.0 * static_cast<double>(busy) / static_cast<double>(total);
    }
private:
    std::optional<CpuTimes> previous_;
};

// Windows gives one GPU Engine instance per process. Sum processes on the
// SAME physical engine before selecting the busiest engine across all GPUs.
class GpuEngineUsage {
public:
    void Add(std::wstring_view instance, double percent)
    {
        const auto start = instance.find(L"luid_");
        const auto type = instance.find(L"_engtype_", start);
        const auto load = UsagePercent(percent);
        if (start == std::wstring_view::npos || type == std::wstring_view::npos
            || instance.substr(start, type - start).find(L"_eng_") == std::wstring_view::npos
            || !load) return;
        auto& total = engines_[std::wstring(instance.substr(start, type - start))];
        total = std::min(total + *load, 100.0);
    }
    std::optional<double> Busiest() const
    {
        std::optional<double> result;
        for (const auto& [engine, percent] : engines_)
            result = std::max(result.value_or(0), percent);
        return result;
    }
private:
    std::map<std::wstring, double> engines_;
};

} // namespace mscharged::platform::detail
