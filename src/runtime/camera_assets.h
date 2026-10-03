#pragma once
#include "resources/camera_animation.h"
#include "Game/Camera/animcam.h"
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace mscharged
{
struct DeleteCameraData { void operator()(cCameraData*) const noexcept; };
// Handles retain game-arena storage. Release every handle before memory shutdown.
// No record is inserted into cAnimCamera's global registry by this owner.
class CameraAsset
{
    std::unique_ptr<cCameraData, DeleteCameraData> data_;
    std::thread::id thread_;
    std::string name_;
    CameraAsset(resources::Bytes file, const std::string& name);
public:
    using Handle = std::shared_ptr<const CameraAsset>;
    static Handle Decode(resources::Bytes file, const std::string& name);
    ~CameraAsset();
    CameraAsset(const CameraAsset&) = delete;
    CameraAsset& operator=(const CameraAsset&) = delete;
    const cCameraData& Data() const;
    const std::string& Name() const { return name_; }
};

// Transactional named storage. Existing handles survive removal/clear. Original
// camera selection and the full asynchronous camera factory remain separate.
class CameraAssetLibrary
{
    std::thread::id thread_ = std::this_thread::get_id();
    std::map<std::string, CameraAsset::Handle> assets_;
    void CheckThread() const;
public:
    CameraAssetLibrary() = default;
    CameraAssetLibrary(const CameraAssetLibrary&) = delete;
    CameraAssetLibrary& operator=(const CameraAssetLibrary&) = delete;
    void Insert(CameraAsset::Handle asset);
    CameraAsset::Handle Find(const std::string& name) const;
    void Erase(const std::string& name);
    void Clear();
};

CameraAsset::Handle LoadCameraAsset(const char* filename, const std::string& name);
class CameraAssetLoad
{
    std::thread::id thread_ = std::this_thread::get_id();
    std::string name_;
    unsigned handle_ = 0;
    bool ready_ = false;
    bool cancelled_ = false;
    CameraAsset::Handle result_;
    std::exception_ptr error_;
    void CheckThread() const;
    static void Complete(void* data, unsigned long size, void* context);
public:
    CameraAssetLoad(const char* filename, const std::string& name);
    ~CameraAssetLoad();
    CameraAssetLoad(const CameraAssetLoad&) = delete;
    CameraAssetLoad& operator=(const CameraAssetLoad&) = delete;
    void Service(); // Pumps original NL completions on the owning thread.
    void Cancel(); // Drains any worker before this request/context can disappear.
    bool Ready() const;
    CameraAsset::Handle Result() const; // Throws pending/cancel/read/decode errors.
};
std::string VerifyStartupCameraAssets();
}
