#include "platform/system_usage.h"
#include "platform/system_usage_detail.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace mscharged::platform;
using namespace mscharged::platform::detail;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    try {
        UsageDelta cpu;
        Check(!cpu.Sample(CpuTimes{100, 1000}), "First sample is unavailable");
        Check(cpu.Sample(CpuTimes{300, 1400}) == 50.0, "All CPUs share the total-time denominator");
        Check(cpu.Sample(CpuTimes{300, 1600}) == 0.0, "Idle is a measured zero");
        Check(cpu.Sample(CpuTimes{500, 1800}) == 100.0, "Fully busy");
        Check(!cpu.Sample(CpuTimes{500, 1800}), "No elapsed time is unavailable");
        Check(!cpu.Sample(CpuTimes{0, 0}), "Counter reset does not underflow");
        Check(cpu.Sample(CpuTimes{20, 100}) == 20.0, "Recovery after counter reset");
        Check(!cpu.Sample(std::nullopt) && !cpu.Sample(CpuTimes{20, 100}), "Missing reading resets baseline");
        Check(!cpu.Sample(CpuTimes{220, 200}), "Inconsistent counters do not fabricate a load");

        std::istringstream stat("cpu 100 20 30 400 50 10 5 15 70 10\ncpu0 1 2 3 4 5 6 7 8\n");
        const auto times = ReadProcCpuTimes(stat);
        Check(times && times->busy == 180 && times->total == 630, "I/O wait idle; guest time not double counted");
        std::istringstream truncated("cpu 10 20"), perCpu("cpu0 1 2 3 4 5 6 7 8");
        Check(!ReadProcCpuTimes(truncated) && !ReadProcCpuTimes(perCpu), "Incomplete/non-total input rejected");
        Check(!UsagePercent(-1) && !UsagePercent(std::numeric_limits<double>::quiet_NaN())
            && !UsagePercent(std::numeric_limits<double>::infinity()), "Invalid driver readings stay unavailable");
        Check(UsagePercent(110) == 100 && UsagePercent(0) == 0, "Bounded percentages preserve zero");

        GpuEngineUsage gpu;
        Check(!gpu.Busiest(), "Missing GPU counters are N/A, not zero");
        gpu.Add(L"pid_10_luid_0x0_0x123_phys_0_eng_0_engtype_3D", 25);
        gpu.Add(L"pid_20_luid_0x0_0x123_phys_0_eng_0_engtype_3D", 45);
        gpu.Add(L"pid_10_luid_0x0_0x123_phys_0_eng_1_engtype_Copy", 60);
        Check(gpu.Busiest() == 70, "External processes add on one engine; independent engines do not add");
        gpu.Add(L"pid_30_luid_0x0_0x456_phys_0_eng_0_engtype_3D", 80);
        Check(gpu.Busiest() == 80, "Different GPUs do not share an engine");
        gpu.Add(L"unexpected", 100);
        gpu.Add(L"pid_40_luid_0x0_0x456_phys_0_eng_0_engtype_3D", -1);
        Check(gpu.Busiest() == 80, "Malformed and invalid instances ignored");
        gpu.Add(L"pid_40_luid_0x0_0x456_phys_0_eng_0_engtype_3D", 30);
        Check(gpu.Busiest() == 100, "Small sampling overlap stays within 100 percent");

        if (argc > 1 && std::string(argv[1]) == "--sample") {
            SystemUsageMonitor monitor;
            for (int n = 0; n < 4; ++n) {
                std::this_thread::sleep_for(std::chrono::milliseconds(550));
                const auto sample = monitor.Latest();
                const auto format = [](std::optional<double> value) { return value ? std::to_string(*value) : "N/A"; };
                std::cout << "CPU " << format(sample.cpu_percent) << " GPU " << format(sample.gpu_percent) << '\n';
                Check(!sample.cpu_percent || (*sample.cpu_percent >= 0 && *sample.cpu_percent <= 100), "Live CPU range");
                Check(!sample.gpu_percent || (*sample.gpu_percent >= 0 && *sample.gpu_percent <= 100), "Live GPU range");
            }
        }
        std::cout << checks << " system usage checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
