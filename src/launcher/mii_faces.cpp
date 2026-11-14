#include "launcher/mii_faces.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace mscharged::launcher
{
namespace mii = platform::mii;

MiiFaces::MiiFaces(Hooks hooks, Limits limits) : hooks_(std::move(hooks)), limits_(limits)
{
    if (!hooks_.load)
        hooks_.load = [](const std::filesystem::path& image) { return mii::ReadMiiPartsFromImage(image); };
    if (!hooks_.render)
        hooks_.render = [](const mii::MiiParts& parts, const mii::MiiFace& face, const mii::MiiHeadOptions& options) {
            return mii::RenderMiiHead(parts, face, options);
        };
    worker_ = std::thread([this] { Work(); });
}

MiiFaces::~MiiFaces()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
    FreeAll();
}

std::string MiiFaces::Key(const mii::MiiFace& face, const Look& look)
{
    // The face's bits alone: a name or a create ID draws no other head.
    std::string key(mii::kMiiRecordBytes, '\0');
    mii::WriteMiiFace(face, key);
    key += char(look.size >> 8);
    key += char(look.size & 0xFF);
    key += char(look.background);
    key += char(look.expression);
    return key;
}

void MiiFaces::SetImage(const std::filesystem::path& image)
{
    if (image == image_) return;
    image_ = image;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        jobs_.clear();
        parts_.reset();
        parts_state_ = Parts::None;
        error_.clear();
        load_pending_ = false;
        drop_textures_ = false;
    }
    if (image.empty()) FreeAll();
}

MiiFaces::Texture MiiFaces::Head(const mii::MiiFace& face, const Look& look, int priority)
{
    if (!mii::ValidMiiFace(face) || look.size < 1 || look.size > 1024) return nullptr;
    const std::string key = Key(face, look);
    if (const auto found = textures_.find(key); found != textures_.end())
    {
        found->second.used = frame_;
        return found->second.texture;
    }
    if (image_.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    if (parts_state_ == Parts::Failed) return nullptr;
    if (parts_state_ == Parts::None)
    {
        parts_state_ = Parts::Loading;
        load_ = image_;
        load_pending_ = true;
    }
    auto [job, added] = jobs_.try_emplace(key);
    if (added)
    {
        job->second.face = face;
        job->second.options.width = job->second.options.height = look.size;
        job->second.options.background = look.background == Background::Favorite
            ? mii::MiiHeadOptions::Background::Favorite
            : mii::MiiHeadOptions::Background::Transparent;
        job->second.options.expression = look.expression;
        job->second.id = next_job_++;
    }
    // Asked for again: this frame's priority.
    if (added || job->second.asked != frame_) job->second.priority = priority;
    else job->second.priority = std::max(job->second.priority, priority);
    job->second.asked = frame_;
    wake_.notify_one();
    return nullptr;
}

MiiFaces::Texture MiiFaces::Head(std::string_view record, const Look& look, int priority)
{
    if (record.size() != mii::kMiiRecordBytes) return nullptr;
    return Head(mii::ReadMiiFace(record), look, priority);
}

MiiFaces::Texture MiiFaces::Cached(const mii::MiiFace& face, const Look& look)
{
    if (!mii::ValidMiiFace(face)) return nullptr;
    const auto found = textures_.find(Key(face, look));
    if (found == textures_.end()) return nullptr;
    found->second.used = frame_;
    return found->second.texture;
}

void MiiFaces::Update()
{
    ++frame_;
    std::vector<std::pair<std::string, std::optional<mii::RgbaImage>>> finished;
    bool drop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drop = std::exchange(drop_textures_, false);
        for (auto job = jobs_.begin(); job != jobs_.end();)
        {
            if (job->second.done)
            {
                finished.emplace_back(job->first, std::move(job->second.image));
                job = jobs_.erase(job);
            }
            // Not asked for in the last two frames: no longer wanted.
            else if (!job->second.rendering && job->second.asked + 2 < frame_)
                job = jobs_.erase(job);
            else
                ++job;
        }
    }
    if (drop) FreeAll();
    for (auto& [key, image] : finished)
    {
        if (drop) break;
        Entry entry;
        entry.used = frame_;
        if (image && hooks_.make)
        {
            entry.texture = hooks_.make(*image);
            if (entry.texture) entry.bytes = image->pixels.size();
        }
        auto [slot, added] = textures_.try_emplace(key, entry);
        if (!added)
        {
            if (slot->second.texture && hooks_.free) hooks_.free(slot->second.texture);
            texture_bytes_ -= slot->second.bytes;
            slot->second = entry;
        }
        texture_bytes_ += entry.bytes;
    }

    // Over the limits: the heads shown least recently go, never one shown
    // in the last frame.
    if (textures_.size() <= limits_.textures && texture_bytes_ <= limits_.bytes) return;
    std::vector<std::map<std::string, Entry>::iterator> idle;
    for (auto entry = textures_.begin(); entry != textures_.end(); ++entry)
        if (entry->second.used + 1 < frame_) idle.push_back(entry);
    std::sort(idle.begin(), idle.end(), [](const auto& a, const auto& b) { return a->second.used < b->second.used; });
    for (const auto& entry : idle)
    {
        if (textures_.size() <= limits_.textures && texture_bytes_ <= limits_.bytes) break;
        if (entry->second.texture && hooks_.free) hooks_.free(entry->second.texture);
        texture_bytes_ -= entry->second.bytes;
        textures_.erase(entry);
    }
}

MiiFaces::Parts MiiFaces::PartsState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return parts_state_;
}

std::string MiiFaces::Error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool MiiFaces::Settled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return parts_state_ != Parts::Loading && !load_pending_ && jobs_.empty();
}

std::size_t MiiFaces::Drawn() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return drawn_;
}

void MiiFaces::FreeAll()
{
    for (const auto& [key, entry] : textures_)
        if (entry.texture && hooks_.free) hooks_.free(entry.texture);
    textures_.clear();
    texture_bytes_ = 0;
}

void MiiFaces::Work()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        // The next head: the highest priority, then the latest asked for.
        const auto next = [&] {
            auto best = jobs_.end();
            for (auto job = jobs_.begin(); job != jobs_.end(); ++job)
                if (!job->second.rendering && !job->second.done
                    && (best == jobs_.end() || job->second.priority > best->second.priority
                        || (job->second.priority == best->second.priority && job->second.asked > best->second.asked)))
                    best = job;
            return best;
        };
        wake_.wait(lock, [&] { return stop_ || load_pending_ || (parts_ && next() != jobs_.end()); });
        if (stop_) return;
        if (load_pending_)
        {
            load_pending_ = false;
            const auto image = load_;
            const auto generation = generation_;
            lock.unlock();
            std::shared_ptr<const mii::MiiParts> parts;
            std::string error;
            try
            {
                parts = std::make_shared<const mii::MiiParts>(hooks_.load(image));
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
            lock.lock();
            if (generation != generation_) continue;
            if (parts)
            {
                parts_ = std::move(parts);
                parts_state_ = Parts::Ready;
            }
            else
            {
                parts_state_ = Parts::Failed;
                error_ = error.empty() ? std::string("The game image has no Mii parts.") : error;
                jobs_.clear();
                drop_textures_ = true;
            }
            continue;
        }
        const auto job = next();
        job->second.rendering = true;
        const std::string key = job->first;
        const auto face = job->second.face;
        const auto options = job->second.options;
        const auto id = job->second.id;
        const auto parts = parts_;
        lock.unlock();
        std::optional<mii::RgbaImage> image;
        try
        {
            image = hooks_.render(*parts, face, options);
        }
        catch (const std::exception&)
        {
        }
        lock.lock();
        ++drawn_;
        if (const auto found = jobs_.find(key); found != jobs_.end() && found->second.id == id)
        {
            found->second.rendering = false;
            found->second.done = true;
            found->second.image = std::move(image);
        }
    }
}
} // namespace mscharged::launcher
