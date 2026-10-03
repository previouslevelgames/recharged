// First real Wii static asset drawn through Aurora GX. This is not glStartup or a game scene.
#include "runtime/scene.h"
#include "runtime/views.h"
#include "runtime/frames.h"
#include "runtime/frame_timing.h"
#include "runtime/cameras.h"
#include "Game/Camera/CameraMan.h"
#include "runtime/shadows.h"
#include "Game/Render/ShadowVolume.h"
#include "resources/compressed_asset.h"
#include "NL/glx/glxTarget.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/static_inventory.h"
#include "runtime/materials.h"
#include "runtime/gpu_readback.h"
#include "runtime/material_environment.h"
#include "resources/static_model.h"
#include "resources/texture_bundle.h"
#include "bootstrap/config.h"
#include "platform/disc.h"
#include "platform/path.h"
#include "mscharged/build_version.h"
#include "Game/Startup.h"
#include "Game/GraphicsMemoryStartup.h"
#include "Game/main.h"
#include "Game/Render/LightingLookup.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/gl.h"
#include "NL/gl/glModel.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include "NL/glx/glxMatrix.h"
#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace mscharged
{
namespace
{
std::atomic_uint backend_errors{0};
void BackendLog(AuroraLogLevel level, const char* module, const char* message, unsigned length)
{
    if (level >= LOG_ERROR) ++backend_errors;
    std::cerr << '[' << module << "] " << std::string_view(message, length) << '\n';
}
struct Session
{
    bool live = false, disc = false, gx = false;
    ~Session()
    {
        if (live) ResetStartupFiles();
        if (disc) aurora_dvd_close();
        if (live)
        {
            if (gx) AuroraGXSync();
            glShutdownMemory();
            ResetStartupMemory(); aurora_shutdown();
        }
    }
};
struct FreeGameBuffer { void operator()(void* data) const { if (data) nlFree(data); } };
struct PendingAsset
{
    std::unique_ptr<void, FreeGameBuffer> data;
    std::size_t size = 0, expected = 0;
    unsigned handle = 0;
    bool done = false;
    ~PendingAsset() { if (handle && !done) nlCancelEntireFileLoad(handle, nullptr); }
    static void Complete(void* data, unsigned long size, void* context)
    {
        auto& asset = *static_cast<PendingAsset*>(context);
        asset.data.reset(data); // Callback owns the buffer, including on validation failure.
        asset.size = size; asset.done = true;
        if (!data || size != asset.expected) throw std::runtime_error("Static asset async read returned an unexpected length");
    }
    void Start(const std::string& name)
    {
        if (name.empty() || name.front() != '/' || name.find('\0') != std::string::npos)
            throw std::runtime_error("Asset paths must be absolute Wii data-partition paths");
        {
            std::unique_ptr<nlFile> file(nlOpen(name.c_str()));
            if (!file) throw std::runtime_error("Cannot open static asset: " + name);
            expected = nlFileSize(file.get(), nullptr);
            if (!expected || expected > resources::MaximumAssetBytes)
                throw std::runtime_error("Static asset is empty or exceeds the 16 MiB preview limit: " + name);
        }
        handle = nlLoadEntireFileAsync(name.c_str(), Complete, this, 32, AllocateEnd, nullptr, 0, nullptr);
        if (!handle && !done) throw std::runtime_error("Cannot queue static asset: " + name);
    }
    resources::Bytes Bytes() const { return {static_cast<const std::uint8_t*>(data.get()), size}; }
};
bool Update()
{
    bool exit = false;
    for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
        if (event->type == AURORA_EXIT) exit = true;
    return exit || SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE];
}
struct Bounds { std::array<float, 3> center{}; float radius = 0; };
Bounds Normalize(resources::StaticModel& model, bool preserve_positions)
{
    std::array<float, 3> low{1e7f, 1e7f, 1e7f}, high{-1e7f, -1e7f, -1e7f};
    for (const auto& packet : model.packets)
        for (const auto& vertex : packet.vertices)
            for (unsigned i = 0; i < 3; ++i)
            { low[i] = std::min(low[i], vertex.position[i]); high[i] = std::max(high[i], vertex.position[i]); }
    Bounds bounds;
    for (unsigned i = 0; i < 3; ++i) bounds.center[i] = (high[i] + low[i]) * 0.5f;
    for (const auto& packet : model.packets)
        for (const auto& vertex : packet.vertices)
        {
            float square = 0;
            for (unsigned i = 0; i < 3; ++i) square += std::pow(vertex.position[i] - bounds.center[i], 2);
            bounds.radius = std::max(bounds.radius, std::sqrt(square));
        }
    if (!std::isfinite(bounds.radius) || bounds.radius < 1e-6f)
        throw std::runtime_error("Static model has degenerate bounds");
    if (!preserve_positions)
        for (auto& packet : model.packets)
            for (auto& vertex : packet.vertices)
                for (unsigned i = 0; i < 3; ++i) vertex.position[i] = (vertex.position[i] - bounds.center[i]) / bounds.radius;
    return bounds;
}
void InvalidateCaches() { GXInvalidateVtxCache(); GXInvalidateTexAll(); }
void DrainGX() { AuroraGXSync(); }
nlVector3 SubmitModel(glModel& model, GLView& submitted, ViewMatrices& matrices,
                      OriginalCameras& cameras, CameraPoseInput& input, float time,
                      const Bounds* authored_bounds)
{
    GXSetCopyClear({24, 28, 34, 255}, GX_MAX_Z24);
    const float radius = authored_bounds ? authored_bounds->radius : 1;
    glMatrixPerspective(matrices.projection, 40 * 3.1415927f / 180,
        float(GXNtsc480IntDf.fbWidth) / GXNtsc480IntDf.efbHeight, 0.1f * radius, 20 * radius);
    nlVector3 camera{0, 0.3f, 4.2f}, center{0, 0, 0};
    nlMatrix4 world;
    if (authored_bounds)
    {
        // Position-generated UVs must retain the asset's coordinate scale.
        // Orbit an explicit camera instead of rewriting vertex positions.
        center = {authored_bounds->center[0], authored_bounds->center[1], authored_bounds->center[2]};
        camera = {center.x + radius * 4.2f * std::sin(time * 0.35f),
                  center.y + radius * 0.3f, center.z + radius * 4.2f * std::cos(time * 0.35f)};
        world.SetIdentity();
    }
    else nlMakeRotationMatrixY(world, time * 0.35f);
    input.position = camera; input.target = center;
    glMatrixLookAt(input.view, camera, center, {0, 1, 0});
    cameras.Advance(0,0); // Caller supplies the diagnostic pose; no gameplay clock.
    matrices.view = cCameraManager::m_matView;
    glModelSetMatrix(&model, world);
    submitted.AttachModel(&model, 0);
    return cCameraManager::m_cameraPosition;
}
}

int RunScenePreview(int argc, char** argv, const std::filesystem::path& config_path, const SceneOptions& options)
{
    std::ofstream logfile;
    auto log = [&](const std::string& message) {
        std::cerr << "[scene] " << message << '\n';
        if (logfile) { logfile << "[scene] " << message << '\n'; logfile.flush(); }
    };
    try
    {
        const auto file = LoadConfig(config_path);
        const auto disc_path = ResolveDiscPath(file.settings, file.path);
        const auto disc = InspectDisc(disc_path);
        if (disc.game_id != "R4QE01" || disc.revision != 1)
            throw std::runtime_error("The static preview currently supports R4QE01 revision 1 only");
        if (file.settings.language != "auto" && file.settings.language != "english"
            && file.settings.language != "french" && file.settings.language != "spanish")
            throw std::runtime_error("Unsupported text language for this USA disc");
        SetStartupSystemLanguage(file.settings.language == "french" ? 3 : file.settings.language == "spanish" ? 4 : 1);
        const char* base = SDL_GetBasePath();
        if (!base) throw std::runtime_error("Cannot locate the executable directory");
        const auto directory = PathFromUtf8(base) / "scene-data";
        std::filesystem::create_directories(directory); logfile.open(directory / "scene.log", std::ios::trunc);
        if (!logfile) throw std::runtime_error("Cannot create scene diagnostic log");
        log(std::string("mscharged ") + build::version + "; decomp " + MSCHARGED_DECOMP_REVISION);
        if (options.shadow_id.has_value() != options.shadow_textures.has_value())
            throw std::invalid_argument("Shadow preview requires both a texture bundle and an ID");
        log("Static material and stadium shadow preview. Full world loading, character animation and game scenes are pending.");
        const auto data_path = PathUtf8(directory);
        AuroraConfig config{};
        config.appName = "Mario Strikers Charged | Static asset preview";
        config.userPath = config.cachePath = data_path.c_str(); config.resourcesPath = base;
        config.desiredBackend = BACKEND_VULKAN; config.enableBackendValidation = true;
        config.windowWidth = 960; config.windowHeight = 720; config.windowPosX = config.windowPosY = -1;
        config.vsync = true; config.logLevel = LOG_WARNING; config.logCallback = BackendLog;
        config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        backend_errors = 0;
        Session session;
        const auto info = aurora_initialize(argc, argv, &config); session.live = true;
        if (!info.window || info.backend != BACKEND_VULKAN) throw std::runtime_error("Static preview requires the actual Vulkan backend");
        ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().LogFilename = nullptr;
        InitializeStartupOS();
        if (!aurora_dvd_open(PathUtf8(disc_path).c_str())) throw std::runtime_error("Cannot mount the Wii data partition");
        session.disc = true; g_Region = 0; InitializeCore();
        const auto mem1_free = StandardAllocator.TotalFreeMemory(), mem2_free = VirtualAllocator.TotalFreeMemory();
        // Original glStartup's memory callback; later graphics stages are pending.
        glInitResourcePools(); InitializeOriginalGraphicsMemory();
        log("Original graphics memory initialized: two MEM1/MEM2 frames, Global resource pool, static GLInventory and 1000 texture indices.");
        InitializeOriginalGraphicsState();
        log("Original GL state and identity matrix initialized; native frame matrix handles and original NL camera math enabled.");
        log("Original InitializeCore completed; loading RLG/RLT through original NL whole-file async services.");
        if (options.world && !options.model_id) throw std::invalid_argument("World preview needs an explicit model ID");
        PendingAsset model_data, texture_data;
        model_data.Start(options.world.value_or(options.model));
        if (!options.world) texture_data.Start(options.textures);
        const auto load_start = std::chrono::steady_clock::now();
        while (!model_data.done || (!options.world && !texture_data.done))
        {
            if (Update()) throw std::runtime_error("Static preview cancelled while loading");
            nlServiceFileSystem();
            if (std::chrono::steady_clock::now() - load_start > std::chrono::seconds(30)) throw std::runtime_error("Static asset load timed out");
            SDL_Delay(1);
        }
        std::vector<resources::StaticModel> models;
        resources::TextureBundle texture_bundle;
        auto& textures = texture_bundle.textures;
        auto& animations = texture_bundle.animations;
        if (options.world)
        {
            auto decoded = resources::InflateAsset(model_data.Bytes());
            auto selected = resources::ReadStaticWorldModel(decoded, *options.model_id);
            std::vector<std::uint32_t> required;
            for (const auto& packet : selected.model.packets)
                for (unsigned i = 0; i < (packet.material.program == 0x112ab470 ? 4u
                    : (packet.material.program == 0x32475c7d || packet.material.program == 0x32bc21e8
                        || packet.material.program == 0x09609a35 || packet.material.program == 0xf2d57ac6
                        || packet.material.program == 0x845cad59) ? 3u
                    : packet.material.program == 0x3eccd955 ? 2u : 1u); ++i)
                    if (std::find(required.begin(), required.end(), packet.material.textures[i].texture) == required.end())
                        required.push_back(packet.material.textures[i].texture);
            texture_bundle = resources::ReadTextureBundle(selected.textures, required);
            models.push_back(std::move(selected.model));
            log(*options.world + ": " + std::to_string(model_data.size) + " compressed bytes -> "
                + std::to_string(decoded.size()) + " checked world bytes; one explicit model selected.");
        }
        else
        {
            log(options.model + ": " + std::to_string(model_data.size) + " bytes; " + options.textures + ": " + std::to_string(texture_data.size) + " bytes.");
            models = resources::ReadStaticModels(model_data.Bytes(), options.model_id);
            texture_bundle = resources::ReadTextureBundle(texture_data.Bytes());
        }
        model_data.data.reset(); texture_data.data.reset();
        auto selected = models.begin();
        if (options.model_id) selected = std::find_if(models.begin(), models.end(), [&](const auto& model) { return model.id == *options.model_id; });
        if (selected == models.end()) throw std::runtime_error("Requested model ID is absent from the RLG collection");
        const auto shadow_packets = std::count_if(selected->packets.begin(), selected->packets.end(),
            [](const auto& p) { return p.material.program == 0x386ecbdd; });
        const bool volume_preview = shadow_packets != 0;
        if (volume_preview && shadow_packets != selected->packets.size())
            throw std::invalid_argument("Mixed shadow-volume and ordinary packets need original world selection");
        if (volume_preview && (options.unlit || options.shadow_id))
            throw std::invalid_argument("Object lighting and projected lookup options do not apply to shadow volumes");
        const bool camera_overlay = std::any_of(selected->packets.begin(), selected->packets.end(),
            [](const auto& p) { return p.material.program == 0x32bc21e8 || p.material.program == 0x845cad59; });
        const auto bounds = Normalize(*selected, camera_overlay);
        const auto selected_id = selected->id;
        std::size_t vertices = 0, indices = 0;
        for (const auto& packet : selected->packets)
        {
            vertices += packet.vertices.size(); indices += packet.indices.size();
            if (std::none_of(textures.begin(), textures.end(), [&](const auto& texture) { return texture.id == packet.material.textures[0].texture; })
                && std::none_of(animations.begin(), animations.end(), [&](const auto& anim) { return anim.id == packet.material.textures[0].texture; }))
                throw std::runtime_error("RLG diffuse texture is missing from the selected RLT bundle");
        }
        std::ostringstream description;
        description << "Selected model 0x" << std::hex << selected->id << std::dec << ": " << selected->packets.size()
            << " packets, " << vertices << " vertices, " << indices << " indices; " << textures.size()
            << " textures, " << animations.size() << " texture animations; original radius " << bounds.radius << '.';
        log(description.str());
        const auto lookup_ids = MaterialLookupTextures(*selected);
        if (!lookup_ids.empty())
        {
            PendingAsset global;
            global.Start("/Art/global.rlt");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!global.done)
            {
                if (Update()) throw std::runtime_error("Static preview cancelled while loading material lookup textures");
                nlServiceFileSystem();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Material texture load timed out");
                SDL_Delay(1);
            }
            auto lookups = resources::ReadTextureBundle(global.Bytes(), lookup_ids);
            if (!lookups.animations.empty()) throw std::runtime_error("Material lookup ramps require static textures");
            for (auto& texture : lookups.textures)
                if (std::none_of(textures.begin(), textures.end(), [&](const auto& old) { return old.id == texture.id; }))
                    textures.push_back(std::move(texture));
            log("Loaded " + std::to_string(lookups.textures.size()) + " original material lookup textures from /Art/global.rlt.");
        }
        if (options.shadow_id)
        {
            PendingAsset shadow_data;
            shadow_data.Start(*options.shadow_textures);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!shadow_data.done)
            {
                if (Update()) throw std::runtime_error("Static preview cancelled while loading shadow lookup");
                nlServiceFileSystem();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Shadow texture load timed out");
                SDL_Delay(1);
            }
            auto shadow_bundle = resources::ReadTextureBundle(shadow_data.Bytes(), {*options.shadow_id});
            const auto& shadow = shadow_bundle.textures;
            if (!shadow_bundle.animations.empty()) throw std::runtime_error("Projected shadow lookup requires a static texture");
            if (shadow.size() != 1 || shadow[0].game_format != GXTex_CI8)
                throw std::runtime_error("Projected shadow lookup requires a CI8/RGB5A3 texture");
            auto existing = std::find_if(textures.begin(), textures.end(), [&](const auto& t) { return t.id == *options.shadow_id; });
            if (existing == textures.end()) textures.push_back(std::move(shadow[0]));
            else if (existing->width != shadow[0].width || existing->height != shadow[0].height ||
                     existing->game_format != shadow[0].game_format || existing->pixels != shadow[0].pixels ||
                     existing->palette != shadow[0].palette)
                throw std::runtime_error("Shadow texture ID conflicts with an existing material texture");
        }
        // Install only the selected model and its material dependencies.
        auto chosen = std::move(*selected); models.clear(); models.push_back(std::move(chosen));
        constexpr std::uint32_t receiver_id = 0xfffffffe;
        if (volume_preview)
        {
            if (selected_id == receiver_id || std::any_of(textures.begin(), textures.end(), [](const auto& t) { return t.id == receiver_id; }))
                throw std::runtime_error("Diagnostic receiver ID conflicts with the selected asset");
            resources::Texture receiver;
            receiver.id=receiver_id; receiver.width=receiver.height=4; receiver.levels=1;
            receiver.game_format=3; receiver.gx_format=6; receiver.bits={8,8,8,0}; receiver.pixels.assign(64,200);
            for(unsigned i=0;i<16;++i) receiver.pixels[i*2]=255;
            textures.push_back(std::move(receiver));
            resources::Packet plane; plane.primitive=0; plane.material.program=0x21db4385;
            plane.material.textures[0]={receiver_id,3}; plane.raster=0xc0007;
            plane.vertices={{{-1.5f,-1.5f,0},{0,0}},{{1.5f,-1.5f,0},{1,0}},{{1.5f,1.5f,0},{1,1}},{{-1.5f,1.5f,0},{0,1}}};
            plane.indices={0,1,2,0,2,3}; models.push_back({receiver_id,{std::move(plane)}});
        }
        VIInit(); VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<std::uint8_t, 65536> fifo{}; GXInit(fifo.data(), fifo.size());
        session.gx = true;
        SetGraphicsCacheInvalidator(InvalidateCaches);
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        MaterialPrograms materials;
        StaticInventory inventory(*glGetCurrentResourcePool(), models, textures, DrainGX, animations);
        GameLighting lighting = DefaultGameLighting();
        lighting.enabled = !options.unlit;
        LightingLookup shadow_lookup;
        bool shadows_enabled = options.shadow_id.has_value();
        if (options.shadow_id)
        {
            shadow_lookup.LoadTexture(*options.shadow_id);
            lighting.shadow.texture = *options.shadow_id;
            lighting.shadow.lookup = &shadow_lookup;
            log("Loaded original projected-shadow lookup: " + std::to_string(shadow_lookup.mWidth) + "x" + std::to_string(shadow_lookup.mHeight) + ".");
        }
        if (!volume_preview)
            log(options.unlit ? "Unlit comparison selected." : "Original key/fill object-light defaults and ambient colour enabled; material lighting flags are preserved.");
        auto* native_model = inventory.Model(selected_id);
        if (!native_model) throw std::runtime_error("Selected model is missing from original GLInventory");
        log("Checked RLG/RLT data installed as pool-owned native glModel/PlatTexture records; drawing original material Activate/Draw/Deactivate and TEV shader recipes through Aurora.");
        ViewMatrices view_matrices;
        OriginalViews views(GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight, DrainGX);
        AuroraFrames backend;
        OriginalFrames lifecycle(backend);
        FrameCounter timing("frame", "send");
        OriginalCameras cameras;
        CameraPoseInput camera_input;
        cCameraManager::PushCamera(&camera_input);
        GLView* submitted = nullptr;
        RLViewCamera shadow_camera;
        std::unique_ptr<ShadowLayers> shadow_layers;
        std::unique_ptr<StadiumShadowVolume> shadow_drawable;
        if (volume_preview)
        {
            shadow_layers = std::make_unique<ShadowLayers>(shadow_camera);
            shadow_drawable = std::make_unique<StadiumShadowVolume>(*native_model);
            glGetBackBufferTarget().target->mClearColour={200,200,200,0};
            log("Original stadium shadow model duplicated and submitted through volume/add/subtract/blend passes. Receiver is diagnostic geometry; full world loading is pending.");
        }
        else
        {
            auto child = std::make_unique<GLView>(&view_matrices, GLRenderPair{}, GLViewSort_Texture);
            child->m_Name = "Static model";
            gRootView.AddChild(child.get());
            submitted = child.release(); // Ownership transfers after the list node is allocated.
        }
        log("Original GLView graph, packet sorting and callback flags connected to Aurora; native target registry initialized.");
        log("Original graphics begin/end/send lifecycle connected; host work drains before frame memory reuse.");
        log("Original camera core evaluates supplied preview poses; authored camera loading and gameplay selection remain pending.");
        bool volume_enabled = true;
        float receiver_height = 0;
        // Render only game-pool records from here; discard host decoder storage.
        models.clear(); models.shrink_to_fit(); textures.clear(); textures.shrink_to_fit();
        animations.clear(); animations.shrink_to_fit();
        unsigned frames = 0, draws = 0, depth_hits = 0, colour_hits = 0, shadow_hits = 0;
        const auto start = std::chrono::steady_clock::now();
        float animation_time = 0;
        while (!options.frames || frames < options.frames)
        {
            if (Update()) break;
            nlServiceFileSystem();
            if (options.frames && std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) throw std::runtime_error("Static preview frame deadline exceeded");
            if (!lifecycle.Acquire()) { SDL_Delay(1); continue; }
            glBeginFrame();
            timing.StartTimer(0);
            const float elapsed = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            glGetCurrentResourcePool()->m_inventory->UpdateTextureAnims(elapsed - animation_time);
            animation_time = elapsed;
            auto frame_lighting = lighting;
            if (!shadows_enabled) frame_lighting.shadow = {};
            backend.time = elapsed;
            backend.lighting = volume_preview ? GameLighting{} : frame_lighting;
            backend.camera_position.reset();
            if (volume_preview)
            {
                GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
                GXSetCopyClear({200,200,200,0}, GX_MAX_Z24);
                nlMatrix4 view, projection, identity, receiver; identity.SetIdentity(); receiver.SetIdentity();
                glMatrixPerspective(projection, 40 * 3.1415927f / 180, 4.0f/3.0f, .1f, 20);
                glMatrixLookAt(view, {0,0,4.2f}, {0,0,0}, {0,1,0});
                camera_input.view = view; camera_input.position = {0,0,4.2f};
                cameras.Advance(0,0);
                shadow_camera.Set(cCameraManager::m_matView, projection);
                shadow_layers->ResetPartitions();
                receiver.m43=receiver_height;
                auto* ground=inventory.Model(receiver_id); glModelSetMatrix(ground, receiver);
                shadow_layers->Layer(eCLV_Shadowed).AttachModel(ground, 0);
                if (volume_enabled) shadow_drawable->Draw(identity);
                RenderShadowVolumeBlend(&shadow_layers->Layer(eCLV_ShadowVolumeBlend));
            }
            else backend.camera_position = SubmitModel(*native_model, *submitted, view_matrices, cameras, camera_input, elapsed,
                                                       camera_overlay ? &bounds : nullptr);
            // Read actual EFB depth, before the ImGui overlay, to require visible geometry.
            // Aurora returns the latest asynchronous snapshot, and returns zero
            // before one is available. Require perspective depth near our camera
            // range, so the initial zero cannot masquerade as rendered geometry.
            bool visible_depth = false;
            for (unsigned y = 1; y <= 3; ++y)
                for (unsigned x = 1; x <= 3; ++x)
                {
                    std::uint32_t depth = 0;
                    GXPeekZ(x * GXNtsc480IntDf.fbWidth / 4, y * GXNtsc480IntDf.efbHeight / 4, &depth);
                    visible_depth = visible_depth || (depth > GX_MAX_Z24 * 0.9 && depth < GX_MAX_Z24);
                }
            if (visible_depth) ++depth_hits;
            ImGui::SetNextWindowPos({16, 16}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.82f);
            ImGui::Begin("Static asset preview", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted("Original Wii mesh and material programs / Aurora GX Vulkan");
            if (volume_preview)
            {
                ImGui::TextUnformatted("Original stadium shadow mesh / diagnostic receiver");
                ImGui::Checkbox("Shadow volume", &volume_enabled);
                ImGui::SliderFloat("Receiver height", &receiver_height, -.8f, .8f);
            }
            else ImGui::Checkbox("Object lighting", &lighting.enabled);
            if (options.shadow_id)
            {
                ImGui::Checkbox("Projected shadow lookup", &shadows_enabled);
                ImGui::SliderFloat2("Shadow scale", lighting.shadow.scale.data(), 0.001f, 2.0f);
                ImGui::SliderFloat2("Shadow offset", lighting.shadow.translation.data(), -1, 1);
            }
            ImGui::TextUnformatted("Full stadium scenes and character animation are pending.");
            ImGui::TextUnformatted("Escape or close the window to exit."); ImGui::End();
            backend.read_colours = (!colour_hits && frames % 15 == 14) || (options.frames && frames + 1 == options.frames);
            glEndFrame();
            timing.StartTimer(1);
            glSendFrame();
            timing.FinishTiming();
            if (backend.read_colours)
            {
                const auto& colours=backend.colours;
                if (aurora_get_stats()->drawCallCount)
                    for (const auto& c : colours)
                    {
                        const int r=volume_preview?200:24, g=volume_preview?200:28, b=volume_preview?200:34;
                        if (std::abs(int(c[0])-r)>3 || std::abs(int(c[1])-g)>3 || std::abs(int(c[2])-b)>3) ++colour_hits;
                        if (volume_preview && std::abs(int(c[0])-78)<=4 && std::abs(int(c[1])-78)<=4
                            && std::abs(int(c[2])-78)<=4) ++shadow_hits;
                    }
            }
            draws += aurora_get_stats()->drawCallCount; ++frames;
        }
        // Explicitly stop services while CPU texture storage and frame FIFO still exist.
        if (glGetCurrentFrame() != static_cast<int>(frames))
            throw std::runtime_error("Original graphics frame counter diverged from submitted frames");
        cameras.Release();
        lifecycle.Release(); views.Release(); shadow_drawable.reset(); shadow_layers.reset(); inventory.Release(); materials.Release(); glShutdownMemory();
        if (StandardAllocator.TotalFreeMemory() != mem1_free || VirtualAllocator.TotalFreeMemory() != mem2_free)
            throw std::runtime_error("Graphics shutdown did not recover both original game arenas");
        log("Original graphics shutdown recovered both game arenas.");
        ResetStartupFiles(); aurora_dvd_close(); session.disc = false;
        ResetStartupMemory(); aurora_shutdown(); session.live = false;
        if (backend_errors || !draws || (!depth_hits && !colour_hits) || (options.frames && frames < options.frames))
            throw std::runtime_error("Static preview incomplete: frames=" + std::to_string(frames) + ", draws=" + std::to_string(draws)
                + ", depth hits=" + std::to_string(depth_hits) + ", colour hits=" + std::to_string(colour_hits) + ", backend errors=" + std::to_string(backend_errors.load()));
        log("Static preview rendered: " + std::to_string(frames) + " frames, " + std::to_string(draws) + " GX draw calls, " + std::to_string(depth_hits) + " geometry depth samples, " + std::to_string(colour_hits) + " visible colour samples. No game scene or gameplay was started.");
        if (volume_preview) log("Original stadium shadow blend samples: " + std::to_string(shadow_hits) + ".");
        return 0;
    }
    catch (const std::exception& error) { log(std::string("FAILED: ") + error.what()); return 1; }
}
}
