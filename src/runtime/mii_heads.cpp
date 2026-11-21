#include "runtime/mii_heads.h"

#include "platform/base64.h"
#include "platform/mii_head.h"
#include "platform/session_log.h"

#include <aurora/imgui.h>

#include <exception>
#include <optional>
#include <string_view>
#include <utility>

namespace mscharged::runtime
{
namespace
{
constexpr std::string_view kPrefix = "miidata:";
// A Mii's record (RFLCharData).
constexpr std::size_t kRecordBytes = 74;
} // namespace

MiiHeads::MiiHeads(std::filesystem::path disc) : disc_(std::move(disc)) {}

MiiHeads::~MiiHeads()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void* MiiHeads::Head(const std::string& key)
{
    // The heads drawn since the last call become textures here.
    std::vector<Drawn> drawn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drawn.swap(drawn_);
    }
    for (auto& head : drawn)
    {
        void* texture = nullptr;
        if (!head.rgba.empty())
            texture = reinterpret_cast<void*>(aurora_imgui_add_texture(std::uint32_t(head.width),
                                                                       std::uint32_t(head.height), head.rgba.data()));
        heads_[head.key] = texture;
    }
    if (const auto known = heads_.find(key); known != heads_.end()) return known->second;
    // A new one: drawn on the worker, unless the run made enough of them or
    // the key holds no record.
    std::optional<std::string> record;
    if (key.compare(0, kPrefix.size(), kPrefix) == 0) record = platform::Base64Decode(key.substr(kPrefix.size()));
    if (heads_.size() >= kMaxHeads || !record || record->size() != kRecordBytes)
    {
        heads_[key] = nullptr;
        return nullptr;
    }
    heads_[key] = nullptr; // asked for: not asked again until drawn
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back(key, std::move(*record));
        if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
    }
    wake_.notify_all();
    return nullptr;
}

void MiiHeads::Run()
{
    namespace mii = platform::mii;
    std::optional<mii::MiiParts> parts;
    bool failed = false;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        auto [key, record] = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        Drawn head{std::move(key)};
        if (!parts && !failed)
        {
            try
            {
                parts = mii::ReadMiiPartsFromImage(disc_);
            }
            catch (const std::exception& error)
            {
                failed = true;
                platform::SessionLogLine(std::string("Menus: players' Miis are not drawn (") + error.what() + ")");
            }
        }
        if (parts)
        {
            try
            {
                mii::MiiHeadOptions options;
                options.width = options.height = 128;
                options.background = mii::MiiHeadOptions::Background::Favorite;
                auto image = mii::RenderMiiHead(*parts, record, options);
                head.width = image.width;
                head.height = image.height;
                head.rgba = std::move(image.pixels);
            }
            catch (const std::exception&)
            {
                // A record that draws no head: the initials stay.
            }
        }
        lock.lock();
        drawn_.push_back(std::move(head));
    }
}
} // namespace mscharged::runtime
