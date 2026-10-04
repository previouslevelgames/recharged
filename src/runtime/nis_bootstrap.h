#pragma once
#include "resources/nis_bootstrap.h"
#include <array>
#include <exception>
#include <memory>
#include <thread>

namespace mscharged
{
struct NisBootstrapAssets
{
    std::vector<std::uint8_t> triggers, animation_proxy;
    std::vector<std::string> no_mirror, no_picture_in_picture;
    std::vector<resources::NisDictionaryEntry> dictionary;
};
// Own the five original bootstrap reads. Readiness means validated text and
// retained bytecode bytes, not executed scripts or ready actors/audio/overlays.
class NisBootstrapLoad
{
    struct Request
    {
        NisBootstrapLoad* owner = nullptr;
        unsigned index = 0, token = 0;
        bool started = false, complete = false;
    };
    std::thread::id thread_ = std::this_thread::get_id();
    std::array<Request, 5> requests_{};
    std::shared_ptr<NisBootstrapAssets> assets_;
    std::exception_ptr error_;
    bool cancelled_ = false, terminal_ = false;
    unsigned completed_mask_ = 0;
    void CheckThread() const;
    void Drain();
    static void Complete(void* bytes, unsigned long size, void* context);
public:
    NisBootstrapLoad();
    ~NisBootstrapLoad();
    NisBootstrapLoad(const NisBootstrapLoad&) = delete;
    NisBootstrapLoad& operator=(const NisBootstrapLoad&) = delete;
    void Poll(); // Observe externally serviced completion and reconcile failures.
    void Service();
    void Cancel(); // Drain pending callbacks before releasing their contexts.
    bool Ready() const;
    unsigned CompletedMask() const;
    std::shared_ptr<const NisBootstrapAssets> Result() const;
};
}
