#include "runtime/camera_assets.h"
#include "runtime/whole_file.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include <cstring>
#include <set>
#include <stdexcept>
#include <type_traits>

std::string mscharged::CanonicalCameraAlias(std::string name)
{
    if (name.empty() || name.size() > 255) throw std::invalid_argument("Camera alias must contain 1..255 ASCII bytes");
    for (auto& c : name)
    {
        if (c < 32 || c >= 127) throw std::invalid_argument("Camera alias must be printable ASCII");
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return name;
}
namespace
{
void CheckFile(const char* filename)
{
    if (!gMemoryInitialized || !nlFileSystemReady()) throw std::logic_error("Camera loading requires game memory and NL files");
    if (!filename || !*filename) throw std::invalid_argument("Camera filename is empty");
    std::unique_ptr<nlFile> file(nlOpen(filename));
    if (!file) throw std::runtime_error("Camera asset is missing: " + std::string(filename));
    if (nlFileSize(file.get(), nullptr) > mscharged::resources::MaximumAssetBytes)
        throw std::length_error("CAM exceeds the 16 MiB asset limit");
}
struct FreeBuffer { void operator()(void* p) const noexcept { nlFree(p); } };
using Buffer = std::unique_ptr<void, FreeBuffer>;
template<class T> T* Allocate(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>);
    return static_cast<T*>(nlMalloc(count * sizeof(T), 8, false));
}
template<class T, std::size_t N> void Copy(T& out, const std::array<float, N>& in)
{
    static_assert(sizeof(T) == sizeof(in) && std::is_trivially_copyable_v<T>);
    std::memcpy(&out, in.data(), sizeof(out));
}
}
namespace mscharged
{
void DeleteCameraData::operator()(cCameraData* data) const noexcept { nlDeleteGameObject(data); }
CameraAsset::CameraAsset(resources::Bytes file, const std::string& name)
    : thread_(std::this_thread::get_id()), name_(CanonicalCameraAlias(name))
{
    if (!gMemoryInitialized) throw std::logic_error("Camera records require initialized game memory");
    const auto decoded = resources::ReadCameraAnimation(file);
    data_.reset(new (8, false) cCameraData);
    auto& data = *data_;
    data.ownsKeyData = true;
    data.m_uHashID = nlStringLowerHash(name_.c_str());
    data.m_uKeyCount = decoded.keys.size();
    data.cameraPos = Allocate<nlVector3>(decoded.keys.size());
    data.targetPos = Allocate<nlVector3>(decoded.keys.size());
    data.cameraRot = Allocate<nlQuaternion>(decoded.keys.size());
    data.fFOV = Allocate<float>(decoded.keys.size());
    data.fFocalLength = Allocate<float>(decoded.keys.size());
    data.m_szName = Allocate<char>(decoded.name.size() + 1);
    std::memcpy(data.m_szName, decoded.name.c_str(), decoded.name.size() + 1);
    for (std::size_t i = 0; i < decoded.keys.size(); ++i)
    {
        const auto& key = decoded.keys[i];
        Copy(data.cameraPos[i], key.position); Copy(data.targetPos[i], key.target); Copy(data.cameraRot[i], key.rotation);
        data.fFOV[i] = key.fov; data.fFocalLength[i] = key.focal_length;
    }
}
CameraAsset::Handle CameraAsset::Decode(resources::Bytes file, const std::string& name)
{ return Handle(new CameraAsset(file, name)); }
CameraAsset::~CameraAsset()
{
    if (!gMemoryInitialized || thread_ != std::this_thread::get_id()) std::terminate();
}
const cCameraData& CameraAsset::Data() const
{
    if (!gMemoryInitialized || thread_ != std::this_thread::get_id())
        throw std::logic_error("Camera record accessed outside its owning memory/thread lifetime");
    return *data_;
}
void CameraAssetLibrary::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("Camera library requires its owner thread");
}
void CameraAssetLibrary::Insert(CameraAsset::Handle asset)
{
    InsertAll({&asset, 1});
}
void CameraAssetLibrary::CheckAvailableAliases(std::span<const std::string> names) const
{
    CheckThread();
    std::set<unsigned long> hashes;
    for (const auto& [name, asset] : assets_) hashes.insert(asset->Data().m_uHashID);
    for (const auto& name : names)
        if (!hashes.insert(nlStringLowerHash(CanonicalCameraAlias(name).c_str())).second)
            throw std::runtime_error("Duplicate camera alias or hash collision");
}
void CameraAssetLibrary::InsertAll(std::span<const CameraAsset::Handle> assets)
{
    CheckThread();
    std::vector<std::string> names;
    names.reserve(assets.size());
    for (const auto& asset : assets)
    {
        if (!asset) throw std::invalid_argument("Cannot insert a null camera asset");
        asset->Data(); // Reject handles from another memory/thread lifetime.
        names.push_back(asset->Name());
    }
    CheckAvailableAliases(names);
    // Map allocation may fail at any point. The destination changes only once
    // every insertion succeeds, retaining the strong exception guarantee.
    auto next = assets_;
    for (const auto& asset : assets) next.emplace(asset->Name(), asset);
    assets_.swap(next);
}
CameraAsset::Handle CameraAssetLibrary::Find(const std::string& name) const
{
    CheckThread();
    const auto found = assets_.find(CanonicalCameraAlias(name));
    return found == assets_.end() ? nullptr : found->second;
}
void CameraAssetLibrary::Erase(const std::string& name) { CheckThread(); assets_.erase(CanonicalCameraAlias(name)); }
void CameraAssetLibrary::Clear() { CheckThread(); assets_.clear(); }
CameraAsset::Handle LoadCameraAsset(const char* filename, const std::string& name)
{
    CanonicalCameraAlias(name); CheckFile(filename);
    unsigned long size = 0;
    Buffer data(nlLoadEntireFile(filename, &size, 32, AllocateEnd, nullptr, 0, nullptr));
    return CameraAsset::Decode({static_cast<const std::uint8_t*>(data.get()), size}, name);
}
void CameraAssetLoad::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("Camera request requires its NL servicing thread");
}
CameraAssetLoad::CameraAssetLoad(const char* filename, const std::string& name) : name_(CanonicalCameraAlias(name))
{
    CheckFile(filename);
    handle_ = nlLoadEntireFileAsync(filename, Complete, this, 32, AllocateEnd, nullptr, 0, nullptr);
    if (!handle_ && !ready_) throw std::runtime_error("Camera load was not queued");
}
CameraAssetLoad::~CameraAssetLoad()
{
    try { Cancel(); } catch (...) { std::terminate(); }
}
void CameraAssetLoad::Complete(void* data, unsigned long size, void* context)
{
    Buffer buffer(data); // The NL callback transfers ownership even on parse failure.
    auto& self = *static_cast<CameraAssetLoad*>(context);
    self.CheckThread();
    self.handle_ = 0;
    self.ready_ = true;
    try { self.result_ = CameraAsset::Decode({static_cast<const std::uint8_t*>(data), size}, self.name_); }
    catch (...) { self.error_ = std::current_exception(); }
}
void CameraAssetLoad::Service()
{
    CheckThread();
    if (Ready()) return;
    try { nlServiceFileSystem(); }
    catch (...)
    {
        auto error = std::current_exception();
        Cancel(); error_ = error;
        std::rethrow_exception(error);
    }
}
void CameraAssetLoad::Cancel()
{
    CheckThread();
    if (ready_) return;
    if (handle_) nlCancelEntireFileLoad(handle_, nullptr);
    handle_ = 0; ready_ = true; cancelled_ = true;
}
bool CameraAssetLoad::Ready() const { CheckThread(); return ready_ || !WholeFileLoadPending(handle_); }
CameraAsset::Handle CameraAssetLoad::Result() const
{
    CheckThread();
    if (!Ready()) throw std::logic_error("Camera asset load is pending");
    if (!ready_) throw std::runtime_error("Camera asset load aborted by the NL file service");
    if (error_) std::rethrow_exception(error_);
    if (cancelled_) throw std::runtime_error("Camera asset load cancelled");
    return result_;
}
}
