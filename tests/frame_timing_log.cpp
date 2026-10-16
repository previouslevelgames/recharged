#include "runtime/frame_timing_log.h"
#include "platform/path.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

using mscharged::runtime::FrameTimingCounters;
using mscharged::runtime::FrameTimingLog;
using mscharged::runtime::FrameTimingRecord;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

struct TestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("mscharged-frame-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { Check(std::filesystem::create_directory(path), "Create owned test directory"); }
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct LegacyEnvironment {
    bool existed = std::getenv("MSCHARGED_FRAME_LOG") != nullptr;
    std::string previous = existed ? std::getenv("MSCHARGED_FRAME_LOG") : "";
    static void Set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s("MSCHARGED_FRAME_LOG", value ? value : "");
#else
        if (value) setenv("MSCHARGED_FRAME_LOG", value, 1);
        else unsetenv("MSCHARGED_FRAME_LOG");
#endif
    }
    ~LegacyEnvironment() { Set(existed ? previous.c_str() : nullptr); }
};

int main()
{
    try {
        TestDirectory directory;
        {
            // 98 frames at 16.7 ms, one 120 ms frame creating pipelines, one 60 ms frame.
            FrameTimingLog log(nullptr, {});
            std::int64_t start = 1'000'000'000;
            for (unsigned n = 0; n < 100; ++n) {
                FrameTimingRecord record;
                record.index = n;
                record.start_ns = start;
                record.duration_ns = n == 40 ? 120'000'000 : n == 70 ? 60'000'000 : 16'700'000;
                record.game_ns = record.duration_ns - 1'000'000;
                record.game_cpu_ns = 4'000'000;
                record.process_cpu_ns = 10'000'000;
                record.presents = 1;
                record.pipelines_created = n == 40 ? 3 : 0;
                log.Add(record);
                start += record.duration_ns;
            }
            const std::string summary = log.Summary();
            std::cout << summary;
            Check(Contains(summary, "100 original frames in 1.8 s"), "Frame count and span");
            Check(Contains(summary, "p50 16.7"), "Median frame time");
            Check(Contains(summary, "max 120.0"), "Longest frame time");
            Check(Contains(summary, "Frames over 20 ms: 2, 34 ms: 2, 50 ms: 2, 100 ms: 1, 250 ms: 0"),
                  "Long-frame thresholds");
            Check(Contains(summary, "game thread 4.00, whole process 10.00"), "CPU per frame");
            Check(Contains(summary, "Pipelines created: 3 in 1 frames; frames over 50 ms with pipeline work: 1/2"),
                  "Pipeline correlation");
            const auto longest = summary.find("Longest frames:");
            Check(longest != std::string::npos && summary.find("      40 ", longest) < summary.find("      70 ", longest),
                  "Longest frames are listed first");
        }
        {
            // Frames tagged with an arena get their own summary after the context.
            FrameTimingLog log(nullptr, {});
            log.SetContext("Session: test\n");
            std::int64_t start = 0;
            for (unsigned n = 0; n < 20; ++n) {
                FrameTimingRecord record;
                record.index = n;
                record.start_ns = start;
                record.duration_ns = 16'700'000;
                record.arena = n < 5 ? -1 : n < 12 ? 13 : 4;
                log.Add(record);
                start += record.duration_ns;
            }
            const std::string summary = log.Summary();
            Check(summary.rfind("Session: test", 0) == 0, "Context leads the summary");
            Check(Contains(summary, "=== Arena: Vice ===") && Contains(summary, "=== Arena: The Dump ==="),
                  "Per-arena sections");
            Check(Contains(summary, "7 original frames") && Contains(summary, "8 original frames"),
                  "Arena frame counts");
            Check(!Contains(summary, "Arena: Menus"), "Menus are not an arena");
        }
        {
            // Begin/End pairs: a frame's duration runs until the next frame starts and
            // counters are differences between consecutive frame ends.
            FrameTimingLog log(nullptr, {});
            log.BeginFrame();
            log.EndFrame(FrameTimingCounters{10, 100, 5, 7, 0, 0});
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            log.BeginFrame();
            Check(log.Records().size() == 1, "The first frame completes when the second starts");
            Check(log.Records()[0].duration_ns >= 3'000'000, "Frame time includes work after the frame");
            Check(log.Records()[0].pipelines_created == 0 && log.Records()[0].vi_fields == 0,
                  "The first frame has no previous counters");
            log.EndFrame(FrameTimingCounters{12, 102, 6, 9, 1, 4096});
            log.BeginFrame();
            const auto& second = log.Records().at(1);
            Check(second.index == 1 && second.presents == 2 && second.vi_fields == 2 && second.pipelines_created == 2,
                  "Counter differences");
            Check(second.pipelines_queued == 1 && second.texture_upload_bytes == 4096 && second.draws == 6,
                  "Per-frame values are copied");
        }
        {
            // The CSV has one header and one row per frame; the summary lands next to it.
            const auto path = directory.path / "explicit.csv";
            {
                FrameTimingLog log(std::fopen(path.string().c_str(), "w"), path.string());
                for (unsigned n = 0; n < 3; ++n) {
                    log.BeginFrame();
                    log.EndFrame(FrameTimingCounters{n, n, 1, 0, 0, 0});
                }
            }
            std::ifstream csv(path);
            std::string line;
            unsigned lines = 0;
            std::getline(csv, line);
            Check(line.rfind("frame,start_ms,duration_ms,", 0) == 0, "CSV header");
            while (std::getline(csv, line)) ++lines;
            Check(lines == 3, "One row per frame, including the last frame at exit");
            csv.close(); // Windows requires the test reader to close before deletion.
            Check(std::filesystem::exists(path.string() + ".summary.txt"), "Summary file");
            std::filesystem::remove(path);
            std::filesystem::remove(path.string() + ".summary.txt");
        }
        {
            // Exercise the actual host factory: the persisted toggle controls
            // logging, and an old environment variable cannot enable or move it.
            LegacyEnvironment restoreEnvironment;
            const auto redirected = directory.path / "old-environment.csv";
            const auto redirectedText = mscharged::PathUtf8(redirected);
            LegacyEnvironment::Set(redirectedText.c_str());
            const auto folder = directory.path / mscharged::PathFromUtf8("Executable space-\xc3\xa9");
            Check(!FrameTimingLog::FromSettings(false, folder), "Disabled logging ignores the old environment switch");
            Check(!std::filesystem::exists(folder) && !std::filesystem::exists(redirected),
                  "Disabled logging creates no output");
            std::filesystem::create_directory(folder);
            std::filesystem::path firstCsv;
            {
                auto log = FrameTimingLog::FromSettings(true, folder);
                Check(bool(log), "Enabled logging opens the executable-directory recording");
                FrameTimingRecord record;
                record.index = 42;
                record.start_ns = 1'000'000'000;
                record.duration_ns = 16'700'000;
                log->Add(record);
                FrameTimingLog::FinishForProcessExit();
                for (const auto& file : std::filesystem::directory_iterator(folder))
                    if (file.path().extension() == ".csv") firstCsv = file.path();
                Check(!firstCsv.empty() && firstCsv.filename().string().rfind("frame-times_", 0) == 0,
                      "Recording has a descriptive timestamped filename in the logs folder");
                auto summary = firstCsv;
                summary += ".summary.txt";
                Check(std::filesystem::exists(summary), "Process-exit hook writes the adjacent summary");
            }
            const auto read = [](const std::filesystem::path& path) {
                std::ifstream input(path);
                return std::string(std::istreambuf_iterator<char>(input), {});
            };
            const auto firstContents = read(firstCsv);
            Check(Contains(firstContents, "\n42,"), "Factory-created recording contains the original frame");
            {
                auto next = FrameTimingLog::FromSettings(true, folder);
                Check(bool(next), "Another recording opens successfully");
            }
            unsigned csvCount = 0;
            for (const auto& file : std::filesystem::directory_iterator(folder))
                csvCount += file.path().extension() == ".csv";
            Check(csvCount == 2 && read(firstCsv) == firstContents,
                  "A new session preserves earlier recordings");
            Check(!std::filesystem::exists(redirected), "Legacy environment cannot redirect enabled logging");
            const auto blocked = directory.path / "not-a-directory";
            std::ofstream(blocked) << "preserve";
            Check(!FrameTimingLog::FromSettings(true, blocked), "An unwritable output location is reported without fallback");
            Check(read(blocked) == "preserve", "Failed logging leaves the blocking file unchanged");
            Check(!FrameTimingLog::FromSettings(false, folder), "Logging can be switched off after an enabled session");
            const auto missing = folder / "logs";
            {
                auto log = FrameTimingLog::FromSettings(true, missing);
                Check(bool(log) && std::filesystem::is_directory(missing), "A missing logs folder is created");
            }
        }
        std::cout << "frame timing log checks passed (" << checks << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
