#pragma once
#include "resources/bytecode.h"
#include <exception>
#include <thread>

namespace mscharged
{
struct BootScriptAsset
{
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<const resources::ScriptBytecode> script;
};
enum class BootScriptLoadState { Loading, Ready, Failed, Cancelled };

// Owns the original LoadingTask script read. Ready means validated bytecode,
// not completed boot services or a ready frontend. Destroy before NL shutdown;
// retained results own their bytes independently of game arenas and this owner.
class BootScriptLoad
{
    std::thread::id thread_ = std::this_thread::get_id();
    std::shared_ptr<const BootScriptAsset> asset_;
    std::exception_ptr error_;
    BootScriptLoadState state_ = BootScriptLoadState::Loading;
    unsigned token_ = 0;
    bool completed_ = false;
    void CheckThread() const;
    void Drain();
    static void Complete(void*, unsigned long, void*);
public:
    BootScriptLoad();
    ~BootScriptLoad();
    BootScriptLoad(const BootScriptLoad&) = delete;
    BootScriptLoad& operator=(const BootScriptLoad&) = delete;
    void Poll();
    void Service();
    void Cancel();
    BootScriptLoadState State() const;
    std::shared_ptr<const BootScriptAsset> Result() const;
};
}
