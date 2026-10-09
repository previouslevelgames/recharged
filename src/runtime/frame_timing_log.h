#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace mscharged::runtime {

// Absolute counters, read by the native loop after each original game frame.
struct FrameTimingCounters {
    std::uint64_t presents = 0;             // successful Present sequence
    std::uint32_t retraces = 0;             // VI retrace count
    std::uint32_t draws = 0;                // draws of the last encoded frame
    std::uint32_t pipelines_created = 0;    // cumulative created pipelines
    std::uint32_t pipelines_queued = 0;     // pipelines still being created
    std::uint32_t texture_upload_bytes = 0; // texture uploads of the last frame
    std::uint64_t fifo_bytes = 0;           // GX FIFO backlog after the frame
};

// One original game frame as seen by the native loop.
struct FrameTimingRecord {
    std::uint64_t index = 0;
    std::int64_t start_ns = 0;       // steady clock (CLOCK_MONOTONIC on Linux)
    std::int64_t duration_ns = 0;    // until the next frame starts (frame time)
    std::int64_t game_ns = 0;        // wall time inside the original frame
    std::int64_t game_cpu_ns = 0;    // CPU time of the calling thread inside it
    std::int64_t process_cpu_ns = 0; // CPU time of all threads during duration_ns
    std::uint32_t vi_fields = 0;     // VI retraces since the previous frame ended
    std::uint32_t presents = 0;      // successful Presents since the previous frame ended
    std::uint32_t draws = 0;
    std::uint32_t pipelines_created = 0;
    std::uint32_t pipelines_queued = 0;
    std::uint32_t texture_upload_bytes = 0;
    std::uint64_t fifo_bytes = 0;
};

// Opt-in per-frame timing log: MSCHARGED_FRAME_LOG=frames.csv writes one CSV
// row per original game frame and, on exit, a summary of the frame-interval
// distribution and the longest frames (frames.csv.summary.txt and stdout).
// Bookkeeping only: it never waits, paces frames or touches game state.
class FrameTimingLog {
public:
    static std::unique_ptr<FrameTimingLog> FromEnvironment();

    // Takes ownership of file; nullptr keeps the records in memory only.
    FrameTimingLog(std::FILE* file, std::string path);
    ~FrameTimingLog();
    FrameTimingLog(const FrameTimingLog&) = delete;
    FrameTimingLog& operator=(const FrameTimingLog&) = delete;

    // Around one original game frame, on the thread that runs it.
    void BeginFrame();
    void EndFrame(const FrameTimingCounters& counters);

    // Completes the log (pending frame, file, summary) once; the destructor
    // calls it. FinishForProcessExit completes the environment log before an
    // _Exit that skips destructors, on the thread that runs the frames.
    void Finish();
    static void FinishForProcessExit();

    // Appends a complete record (EndFrame and tests).
    void Add(const FrameTimingRecord& record);
    std::string Summary() const;
    const std::vector<FrameTimingRecord>& Records() const { return records_; }

private:
    std::FILE* file_;
    std::string path_;
    bool finished_ = false;
    std::vector<FrameTimingRecord> records_;
    std::uint64_t next_index_ = 0;
    bool has_previous_ = false;
    FrameTimingCounters previous_counters_{};
    std::int64_t frame_start_ns_ = 0;
    std::int64_t frame_thread_cpu_ns_ = 0;
    std::int64_t frame_process_cpu_ns_ = 0;
    // EndFrame leaves the record pending until the next frame starts.
    bool pending_valid_ = false;
    FrameTimingRecord pending_{};
    std::int64_t pending_process_cpu_ns_ = 0;
};

} // namespace mscharged::runtime
