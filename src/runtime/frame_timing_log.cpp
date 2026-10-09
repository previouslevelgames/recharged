#include "runtime/frame_timing_log.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <numeric>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <ctime>
#endif

namespace mscharged::runtime {
namespace {

std::int64_t SteadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#if defined(_WIN32)
std::int64_t FileTimeNs(const FILETIME& time)
{
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) *
           100;
}

// Windows updates these per scheduler tick, so short frames read as 0 or ~15 ms.
std::int64_t ThreadCpuNs()
{
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
    return FileTimeNs(kernel) + FileTimeNs(user);
}

std::int64_t ProcessCpuNs()
{
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
    return FileTimeNs(kernel) + FileTimeNs(user);
}
#else
std::int64_t ClockNs(clockid_t clock)
{
    timespec time{};
    if (clock_gettime(clock, &time) != 0) return 0;
    return static_cast<std::int64_t>(time.tv_sec) * 1000000000 + time.tv_nsec;
}

std::int64_t ThreadCpuNs() { return ClockNs(CLOCK_THREAD_CPUTIME_ID); }
std::int64_t ProcessCpuNs() { return ClockNs(CLOCK_PROCESS_CPUTIME_ID); }
#endif

double Ms(std::int64_t ns) { return static_cast<double>(ns) / 1e6; }

constexpr std::size_t kMaxRecords = 4u << 20; // about 4.5 hours at 60 frames/s

FrameTimingLog* g_environment_log = nullptr;

} // namespace

std::unique_ptr<FrameTimingLog> FrameTimingLog::FromEnvironment()
{
    const char* path = std::getenv("MSCHARGED_FRAME_LOG");
    if (!path || !*path) return nullptr;
    std::FILE* file = std::fopen(path, "w");
    if (!file) {
        std::fprintf(stderr, "Frame timing log: cannot open %s\n", path);
        return nullptr;
    }
    std::setvbuf(file, nullptr, _IOFBF, 1 << 16);
    std::fprintf(stderr, "Frame timing log: writing %s\n", path);
    auto log = std::make_unique<FrameTimingLog>(file, path);
    g_environment_log = log.get();
    return log;
}

void FrameTimingLog::FinishForProcessExit()
{
    if (g_environment_log) g_environment_log->Finish();
}

FrameTimingLog::FrameTimingLog(std::FILE* file, std::string path) : file_(file), path_(std::move(path))
{
    if (file_)
        std::fputs("frame,start_ms,duration_ms,game_ms,game_cpu_ms,process_cpu_ms,vi_fields,presents,draws,"
                   "pipelines_created,pipelines_queued,texture_upload_kb,fifo_mib,steady_ns\n",
                   file_);
}

FrameTimingLog::~FrameTimingLog()
{
    Finish();
    if (g_environment_log == this) g_environment_log = nullptr;
}

void FrameTimingLog::Finish()
{
    if (finished_) return;
    finished_ = true;
    if (pending_valid_) {
        // The loop ended: no next frame start, so the frame time is its own.
        pending_.duration_ns = pending_.game_ns;
        pending_.process_cpu_ns = ProcessCpuNs() - pending_process_cpu_ns_;
        Add(pending_);
        pending_valid_ = false;
    }
    if (file_) std::fclose(file_);
    file_ = nullptr;
    if (path_.empty()) return;
    const std::string summary = Summary();
    std::fputs(summary.c_str(), stdout);
    std::fflush(stdout);
    if (std::FILE* out = std::fopen((path_ + ".summary.txt").c_str(), "w")) {
        std::fputs(summary.c_str(), out);
        std::fclose(out);
    }
}

void FrameTimingLog::BeginFrame()
{
    if (finished_) return;
    const std::int64_t now = SteadyNs();
    const std::int64_t process = ProcessCpuNs();
    if (pending_valid_) {
        pending_.duration_ns = now - pending_.start_ns;
        pending_.process_cpu_ns = process - pending_process_cpu_ns_;
        Add(pending_);
        pending_valid_ = false;
    }
    frame_start_ns_ = now;
    frame_process_cpu_ns_ = process;
    frame_thread_cpu_ns_ = ThreadCpuNs();
}

void FrameTimingLog::EndFrame(const FrameTimingCounters& counters)
{
    if (finished_) return;
    const std::int64_t end = SteadyNs();
    FrameTimingRecord record;
    record.index = next_index_++;
    record.start_ns = frame_start_ns_;
    record.game_ns = end - frame_start_ns_;
    record.game_cpu_ns = ThreadCpuNs() - frame_thread_cpu_ns_;
    record.draws = counters.draws;
    record.pipelines_queued = counters.pipelines_queued;
    record.texture_upload_bytes = counters.texture_upload_bytes;
    record.fifo_bytes = counters.fifo_bytes;
    if (has_previous_) {
        record.vi_fields = counters.retraces - previous_counters_.retraces;
        record.presents = static_cast<std::uint32_t>(counters.presents - previous_counters_.presents);
        record.pipelines_created = counters.pipelines_created - previous_counters_.pipelines_created;
    }
    previous_counters_ = counters;
    has_previous_ = true;
    pending_ = record;
    pending_process_cpu_ns_ = frame_process_cpu_ns_;
    pending_valid_ = true;
}

void FrameTimingLog::Add(const FrameTimingRecord& record)
{
    if (records_.size() < kMaxRecords) records_.push_back(record);
    if (!file_) return;
    const std::int64_t origin = records_.empty() ? record.start_ns : records_.front().start_ns;
    std::fprintf(file_,
                 "%" PRIu64 ",%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                 ",%.1f,%.2f,%" PRId64 "\n",
                 record.index, Ms(record.start_ns - origin), Ms(record.duration_ns), Ms(record.game_ns),
                 Ms(record.game_cpu_ns), Ms(record.process_cpu_ns), record.vi_fields, record.presents, record.draws,
                 record.pipelines_created, record.pipelines_queued, record.texture_upload_bytes / 1024.0,
                 record.fifo_bytes / 1048576.0, record.start_ns);
    if (record.index % 60 == 59) std::fflush(file_);
}

std::string FrameTimingLog::Summary() const
{
    std::string text;
    char line[256];
    const auto append = [&](const char* format, auto... values) {
        std::snprintf(line, sizeof line, format, values...);
        text += line;
    };
    if (records_.size() < 2) return "Frame timing: fewer than two frames recorded\n";

    const auto& first = records_.front();
    const auto& last = records_.back();
    const double seconds = static_cast<double>(last.start_ns + last.duration_ns - first.start_ns) / 1e9;
    std::uint64_t presents = 0, pipelines = 0, pipelineFrames = 0;
    std::vector<std::int64_t> durations;
    durations.reserve(records_.size());
    for (const auto& record : records_) {
        durations.push_back(record.duration_ns);
        presents += record.presents;
        pipelines += record.pipelines_created;
        pipelineFrames += record.pipelines_created != 0;
    }
    append("Frame timing: %zu original frames in %.1f s (%.1f frames/s), %" PRIu64 " presents (%.1f/s)\n",
           records_.size(), seconds, records_.size() / seconds, presents, presents / seconds);

    std::vector<std::int64_t> sorted = durations;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double p) {
        const std::size_t rank = static_cast<std::size_t>(p / 100.0 * sorted.size() + 0.999999);
        return Ms(sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1]);
    };
    append("Frame time ms: p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", percentile(50), percentile(90),
           percentile(99), percentile(99.9), Ms(sorted.back()));
    const auto over = [&](double ms) {
        return std::count_if(durations.begin(), durations.end(), [&](std::int64_t d) { return Ms(d) > ms; });
    };
    append("Frames over 20 ms: %td, 34 ms: %td, 50 ms: %td, 100 ms: %td, 250 ms: %td\n", over(20), over(34),
           over(50), over(100), over(250));
    const std::int64_t gameCpu = std::accumulate(records_.begin(), records_.end(), std::int64_t{0},
                                                 [](std::int64_t sum, const auto& r) { return sum + r.game_cpu_ns; });
    const std::int64_t processCpu =
        std::accumulate(records_.begin(), records_.end(), std::int64_t{0},
                        [](std::int64_t sum, const auto& r) { return sum + r.process_cpu_ns; });
    append("CPU per frame ms: game thread %.2f, whole process %.2f\n", Ms(gameCpu) / records_.size(),
           Ms(processCpu) / records_.size());
    std::size_t slowWithPipelines = 0, slow = 0;
    for (const auto& record : records_)
        if (Ms(record.duration_ns) > 50) {
            ++slow;
            slowWithPipelines += record.pipelines_created != 0 || record.pipelines_queued != 0;
        }
    append("Pipelines created: %" PRIu64 " in %" PRIu64 " frames; frames over 50 ms with pipeline work: %zu/%zu\n",
           pipelines, pipelineFrames, slowWithPipelines, slow);

    std::vector<std::size_t> order(records_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const std::size_t shown = std::min<std::size_t>(20, order.size());
    std::partial_sort(order.begin(), order.begin() + shown, order.end(),
                      [&](std::size_t a, std::size_t b) { return records_[a].duration_ns > records_[b].duration_ns; });
    append("Longest frames:\n%8s %9s %9s %9s %9s %9s %4s %5s %9s %9s\n", "frame", "t_s", "frame_ms", "game_ms",
           "cpu_ms", "proc_ms", "vi", "draws", "pipelines", "tex_kb");
    for (std::size_t n = 0; n < shown; ++n) {
        const auto& r = records_[order[n]];
        append("%8" PRIu64 " %9.3f %9.1f %9.1f %9.1f %9.1f %4" PRIu32 " %5" PRIu32 " %4" PRIu32 "+%-4" PRIu32
               " %9.1f\n",
               r.index, static_cast<double>(r.start_ns - first.start_ns) / 1e9, Ms(r.duration_ns), Ms(r.game_ns),
               Ms(r.game_cpu_ns), Ms(r.process_cpu_ns), r.vi_fields, r.draws, r.pipelines_created,
               r.pipelines_queued, r.texture_upload_bytes / 1024.0);
    }
    return text;
}

} // namespace mscharged::runtime
