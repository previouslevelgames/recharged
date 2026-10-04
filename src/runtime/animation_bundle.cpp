#include "runtime/animation_bundle.h"
#include "runtime/whole_file.h"
#include "resources/chunk_reader.h"
#include "resources/compressed_asset.h"
#include "Game/SHierarchy.h"
#include "Game/SAnim.h"
#include "Game/SAnim/AnimRetargeter.h"
#include "NL/MemAlloc.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include <algorithm>
#include <exception>
#include <optional>
#include <thread>

namespace mscharged
{
namespace
{
#include "mscharged/character_animation_profiles.inc"
}
const CharacterAnimationProfile& CharacterAnimation(unsigned index) { return character_profiles.at(index); }
AnimationBundle::Handle AnimationBundle::Decode(resources::Bytes resident, resources::Bytes temporary,
    std::uint32_t hierarchy_hash)
{
    using namespace resources;
    auto bundle = std::shared_ptr<AnimationBundle>(new AnimationBundle);
    bool have_hierarchy = false, selected = false;
    std::optional<std::uint32_t> signature;
    // The current set is deliberately not reset between the two files:
    // World::LoadData resets m_pAnimationSet once, then calls LoadChunks twice.
    for (const auto file : {resident, temporary})
    {
        Require(!file.empty() && file.size() <= MaximumAssetBytes, "Animation world is empty or exceeds 16 MiB");
        const auto root = ReadChunk(file, 0, file.size());
        Require(root.id == 0x80000001 && root.next == file.size(), "Invalid animation world root");
        auto pos = std::size_t(root.payload.data() - file.data());
        const auto end = pos + root.payload.size();
        while (pos < end)
        {
            const auto chunk = ReadChunk(file, pos, end);
            Require(chunk.next <= end, "Animation world chunk padding exceeds its parent");
            if (chunk.id == 0x80018000)
            {
                // Decode every hierarchy that establishes an association; a
                // stale export pointer is never a signature or a host pointer.
                auto hierarchy = HierarchyAsset::Decode(file, pos, chunk.next);
                have_hierarchy = true;
                selected = hierarchy->Data().GetHashID() == hierarchy_hash;
                if (selected)
                {
                    if (bundle->hierarchy_)
                        throw UnsupportedResource("Repeated hierarchy identity needs qualified animation-set merging");
                    bundle->hierarchy_ = std::move(hierarchy);
                }
            }
            else if (chunk.id == 0x80017000)
            {
                Require(have_hierarchy, "Animation precedes its world hierarchy");
                if (selected)
                {
                    Require(bundle->animations_.size() < MaximumSAnimTracks, "Animation set exceeds 256 tracks");
                    auto animation = SAnimAsset::Decode(file, pos, chunk.next);
                    const auto& data = animation->Data();
                    const auto current_signature = std::uint32_t(data.m_nHierarchySignature);
                    if (signature && *signature != current_signature)
                        throw UnsupportedResource("Mixed animation hierarchy signatures require qualified retarget maps");
                    signature = current_signature;
                    Require(data.m_nNumNodes >= unsigned(bundle->hierarchy_->Data().GetNumNodes()),
                        "Animation cannot address every authored hierarchy node without a retarget map");
                    bundle->animations_.push_back(std::move(animation));
                }
            }
            else if (chunk.id == 0x80017104 && selected)
                throw UnsupportedResource("Animation retarget records are not selected by the direct world profile");
            pos = chunk.next;
        }
    }
    Require(bundle->hierarchy_ != nullptr, "Requested hierarchy is absent from the authored world");
    Require(!bundle->animations_.empty(), "Requested hierarchy has no authored animation tracks");
    return bundle;
}
AnimationBundle::Handle AnimationBundle::DecodeCharacter(resources::Bytes hierarchy, resources::Bytes animations,
    resources::Bytes retargets, unsigned character)
{
    const auto& profile = CharacterAnimation(character);
    auto bundle = std::shared_ptr<AnimationBundle>(new AnimationBundle);
    bundle->hierarchy_ = HierarchyAsset::Decode(hierarchy);
    const auto& target = bundle->hierarchy_->Data();
    if (target.GetHashID() != nlStringHash(profile.name.data()) || profile.name != target.m_szName)
        throw std::runtime_error("Character hierarchy does not match its original template identity");
    AnimationRetargetAssets lists(retargets);
    bundle->retargets_ = lists.Selected();
    const auto& list = bundle->retargets_->Data();
    if (!list.m_NumAnimRetargets) throw std::runtime_error("Character retarget list is empty");
    for (long i = 0; i < list.m_NumAnimRetargets; ++i)
        if (list.m_pAnimRetarget[i].m_Unknown08 != unsigned(target.GetNumNodes()))
            throw std::runtime_error("Character retarget inventory does not describe its target hierarchy");
    SAnimAssets tracks(animations);
    std::optional<unsigned long> direct_signature;
    for (std::size_t i = 0; i < tracks.Size(); ++i)
    {
        auto animation = tracks.At(i);
        const auto& data = animation->Data();
        const auto* map = bundle->retargets_->Find(data);
        if (map) map = &bundle->retargets_->RequireMap(data, target.GetNumNodes());
        else
        {
            // CharacterLoader selected this hierarchy and FE file together;
            // GetCharacterAnimRetarget returns null when this native rig's
            // signature needs no conversion. Do not infer that relationship
            // for arbitrary files or permit multiple unmatched signatures.
            if (data.m_nNumNodes < unsigned(target.GetNumNodes())
                || (direct_signature && *direct_signature != data.m_nHierarchySignature))
                throw std::runtime_error("Character animation lacks a required retarget map");
            direct_signature = data.m_nHierarchySignature;
        }
        bundle->animations_.push_back(std::move(animation)); bundle->maps_.push_back(map);
    }
    return bundle;
}
SAnimAsset::Handle AnimationBundle::Find(std::uint32_t hash) const
{
    for (auto i = animations_.rbegin(); i != animations_.rend(); ++i)
        if ((*i)->Data().GetHashID() == hash) return *i;
    return {};
}
std::size_t AnimationBundle::AnimationNode(std::size_t node, bool mirror) const
{
    if (retargets_) throw std::logic_error("Character mapping requires an explicit animation track");
    const auto& hierarchy = hierarchy_->Data();
    if (node >= std::size_t(hierarchy.GetNumNodes())) throw std::out_of_range("Animation hierarchy node is out of bounds");
    return mirror ? std::size_t(hierarchy.GetMirroredNode(int(node))) : node;
}
const AnimRetarget* AnimationBundle::Retarget(std::size_t animation) const
{
    animations_.at(animation);
    return maps_.empty() ? nullptr : maps_.at(animation);
}
std::optional<std::size_t> AnimationBundle::MappedNode(std::size_t animation, std::size_t node, bool mirror) const
{
    const auto* map = Retarget(animation);
    const auto& hierarchy = hierarchy_->Data();
    if (node >= std::size_t(hierarchy.GetNumNodes())) throw std::out_of_range("Animation hierarchy node is out of bounds");
    const auto index = mirror ? std::size_t(hierarchy.GetMirroredNode(int(node))) : node;
    if (!map) return index;
    const auto source = map->m_pMap[index];
    if (source == -1) return std::nullopt;
    return std::size_t(source);
}

struct AnimationBundleLoad::Implementation
{
    struct Request { Implementation* owner; unsigned index, token = 0; bool complete = false; };
    const std::thread::id thread = std::this_thread::get_id();
    AnimationBundleRequest selection;
    std::optional<unsigned> character;
    unsigned request_count = 2;
    std::array<std::string, 3> paths;
    std::array<Request, 3> requests{{{this, 0}, {this, 1}, {this, 2}}};
    std::array<std::size_t, 3> sizes{};
    std::array<std::vector<std::uint8_t>, 3> bytes;
    std::size_t retained_bytes = 0;
    AnimationBundle::Handle current;
    std::exception_ptr error;
    AnimationBundleState state = AnimationBundleState::Idle;
    bool servicing = false;
    const std::string& Path(unsigned i) const { return paths[i]; }
    auto Requests() { return std::span(requests).first(request_count); }
    void CheckThread() const
    {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Animation bundles require their NL servicing thread");
    }
    void CheckMutation() const
    {
        CheckThread();
        if (servicing) throw std::logic_error("Animation bundle mutation during NL service is not supported");
    }
    void Drain()
    {
        for (auto& request : requests)
        {
            if (request.token) nlCancelEntireFileLoad(request.token, nullptr);
            request.token = 0;
        }
        for (auto& file : bytes) std::vector<std::uint8_t>().swap(file);
    }
    void Start()
    {
        if (!gMemoryInitialized || !nlFileSystemReady())
            throw std::logic_error("Animation bundles require initialized memory and NL files");
        std::size_t total = 0;
        for (unsigned i = 0; i < request_count; ++i)
        {
            const auto& path = Path(i);
            if (path.empty() || path.find('\0') != std::string::npos)
                throw std::invalid_argument("Animation world path is empty or contains NUL");
            std::unique_ptr<nlFile> file(nlOpen(path.c_str()));
            if (!file) throw std::runtime_error("Animation world is missing: " + path);
            sizes[i] = nlFileSize(file.get(), nullptr);
            if (!sizes[i] || sizes[i] > resources::MaximumAssetBytes || sizes[i] > MaximumRetainedBytes - total)
                throw std::length_error("Animation world read exceeds its batch limits");
            total += sizes[i];
        }
        for (auto& request : Requests())
        {
            request.token = nlLoadEntireFileAsync(Path(request.index).c_str(), Complete, &request,
                32, AllocateEnd, nullptr, 0, &VirtualAllocator);
            if (!request.token && !request.complete) throw std::runtime_error("Animation world read was not queued");
            if (error) std::rethrow_exception(error);
        }
    }
    static void Complete(void* data, unsigned long size, void* context)
    {
        const auto free = [](void* buffer) { nlFree(buffer); };
        std::unique_ptr<void, decltype(free)> buffer(data, free);
        auto& request = *static_cast<Request*>(context);
        auto& load = *request.owner;
        load.CheckThread(); request.token = 0; request.complete = true;
        if (load.error) return;
        try
        {
            if (size != load.sizes[request.index]) throw std::runtime_error("Animation world changed size during its read");
            const resources::Bytes input{static_cast<const std::uint8_t*>(data), size};
            const bool compressed = load.Path(request.index).ends_with(".zlib");
            const std::size_t decoded_size = compressed ? resources::U32(input, 0) : size;
            if (!decoded_size || decoded_size > resources::MaximumAssetBytes
                || decoded_size > MaximumRetainedBytes - load.retained_bytes)
                throw std::length_error("Decoded animation world exceeds its batch limits");
            auto& destination = load.bytes[request.index];
            if (compressed) destination = resources::InflateAsset(input);
            else destination.assign(input.begin(), input.end());
            load.retained_bytes += destination.size();
        }
        catch (...) { load.error = std::current_exception(); }
    }
};
AnimationBundleLoad::AnimationBundleLoad() : impl_(std::make_unique<Implementation>()) {}
AnimationBundleLoad::~AnimationBundleLoad() { try { Cancel(); } catch (...) { std::terminate(); } }
void AnimationBundleLoad::Begin(AnimationBundleRequest request)
{ BeginImpl(std::move(request), {}); }
void AnimationBundleLoad::BeginCharacter(unsigned index)
{
    impl_->CheckMutation(); CharacterAnimation(index); BeginImpl({}, index);
}
void AnimationBundleLoad::BeginImpl(AnimationBundleRequest request, std::optional<unsigned> character)
{
    auto& s = *impl_; s.CheckMutation(); s.Drain();
    s.selection = std::move(request); s.character = character; s.error = {}; s.retained_bytes = 0;
    for (auto& r : s.requests) r.complete = false;
    s.state = AnimationBundleState::Loading;
    try
    {
        s.request_count = character ? 3 : 2;
        if (character)
        {
            const auto& profile = CharacterAnimation(*character);
            s.paths = {std::string(profile.hierarchy_path), std::string(profile.animation_path), std::string(profile.retarget_path)};
        }
        else s.paths = {s.selection.resident, s.selection.temporary, {}};
        s.Start();
    }
    catch (...) { s.error = std::current_exception(); s.Drain(); s.state = AnimationBundleState::Failed; throw; }
}
void AnimationBundleLoad::Poll()
{
    auto& s = *impl_; s.CheckMutation();
    if (s.state != AnimationBundleState::Loading) return;
    try
    {
        if (s.error) std::rethrow_exception(s.error);
        for (const auto& r : s.Requests())
            if (!r.complete && !WholeFileLoadPending(r.token))
                throw std::runtime_error("Animation world read failed or file services stopped: " + s.Path(r.index));
        if (std::all_of(s.Requests().begin(), s.Requests().end(), [](const auto& r) { return r.complete; }))
        {
            auto next = s.character ? AnimationBundle::DecodeCharacter(s.bytes[0], s.bytes[1], s.bytes[2], *s.character)
                : AnimationBundle::Decode(s.bytes[0], s.bytes[1], s.selection.hierarchy_hash);
            s.current = std::move(next); s.Drain(); s.state = AnimationBundleState::Ready;
        }
    }
    catch (...) { s.error = std::current_exception(); s.Drain(); s.state = AnimationBundleState::Failed; }
}
void AnimationBundleLoad::Service()
{
    Poll(); if (impl_->state != AnimationBundleState::Loading) return;
    impl_->servicing = true;
    try { nlServiceFileSystem(); }
    catch (...) { impl_->servicing = false; Poll(); throw; }
    impl_->servicing = false;
    Poll();
}
void AnimationBundleLoad::Cancel()
{
    auto& s = *impl_; s.CheckMutation();
    if (s.state == AnimationBundleState::Loading) { s.Drain(); s.state = AnimationBundleState::Cancelled; }
}
void AnimationBundleLoad::Unload()
{
    Cancel(); auto& s = *impl_; s.current.reset(); s.error = {}; s.state = AnimationBundleState::Idle;
}
AnimationBundleState AnimationBundleLoad::State() const { impl_->CheckThread(); return impl_->state; }
unsigned AnimationBundleLoad::CompletedFiles() const
{
    impl_->CheckThread();
    return std::count_if(impl_->requests.begin(), impl_->requests.end(), [](const auto& r) { return r.complete; });
}
AnimationBundle::Handle AnimationBundleLoad::Current() const { impl_->CheckThread(); return impl_->current; }
AnimationBundle::Handle AnimationBundleLoad::Result() const
{
    impl_->CheckThread(); if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->state != AnimationBundleState::Ready) throw std::logic_error("Animation bundle is not ready");
    return impl_->current;
}
}
