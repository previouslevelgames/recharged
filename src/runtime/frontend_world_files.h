#pragma once
#include "resources/binary_reader.h"
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mscharged
{
struct FrontendWorldFiles
{
    std::vector<std::uint8_t> resident, temporary;
    std::string tweaks;
};
enum class FrontendWorldFileState { Resident, Temporary, Tweaks, Ready, Failed, Cancelled };
// The original FE main world paths and resident-before-temporary load ordering.
// Completion supplies decompressed files and validated INI text, without marking
// world objects, effects or tweak bindings initialized. No borrowed Wii buffers.
class FrontendWorldFileLoad
{
    std::thread::id thread_ = std::this_thread::get_id();
    std::shared_ptr<FrontendWorldFiles> files_;
    FrontendWorldFileState state_ = FrontendWorldFileState::Resident;
    unsigned token_ = 0;
    bool complete_ = false;
    std::exception_ptr error_;
    void CheckThread() const;
    void Start();
    void Drain();
    static void Complete(void*, unsigned long, void*);
public:
    FrontendWorldFileLoad();
    ~FrontendWorldFileLoad();
    FrontendWorldFileLoad(const FrontendWorldFileLoad&) = delete;
    FrontendWorldFileLoad& operator=(const FrontendWorldFileLoad&) = delete;
    void Poll();
    void Service();
    void Cancel();
    FrontendWorldFileState State() const;
    std::shared_ptr<const FrontendWorldFiles> Result() const;
};
}
