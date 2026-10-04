// First real Wii static asset drawn through Aurora GX. This is not glStartup or a game scene.
#include "runtime/scene.h"
#include "runtime/views.h"
#include "runtime/frames.h"
#include "runtime/frame_timing.h"
#include "runtime/cameras.h"
#include "runtime/animated_camera.h"
#include "runtime/debug_camera_input.h"
#include "runtime/nis_pip_scene.h"
#include "Game/Camera/CameraMan.h"
#include "runtime/shadows.h"
#include "Game/Render/ShadowVolume.h"
#include "resources/compressed_asset.h"
#include "resources/world_scene.h"
#include "runtime/world_objects.h"
#include "runtime/world_render.h"
#include "runtime/frontend_world_files.h"
#include "runtime/frontend_visuals.h"
#include "runtime/frontend_font_registry.h"
#include "runtime/frontend_packets.h"
#include "runtime/frontend_images.h"
#include "runtime/frontend_session.h"
#include "runtime/frontend_handler.h"
#include "runtime/frontend_boot_loading.h"
#include "runtime/particle_files.h"
#include "runtime/particle_controller.h"
#include "runtime/particle_controller_render.h"
#include "runtime/frontend_input_sdl.h"
#include "resources/frontend_text_catalog.h"
#include "resources/frontend_instances.h"
#include "NL/glx/glxTarget.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_startup.h"
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
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
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
    const bool focused = aurora_get_window() && (SDL_GetWindowFlags(aurora_get_window()) & SDL_WINDOW_INPUT_FOCUS);
    return exit || (focused && !ImGui::GetIO().WantCaptureKeyboard && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE]);
}
using Bounds = resources::SceneBounds;
class ScenePool
{
    GLResourcePool* previous_ = glGetCurrentResourcePool();
public:
    explicit ScenePool(GLResourcePool* pool) { glSetCurrentResourcePool(pool); }
    ~ScenePool() { Restore(); }
    void Restore() { if (previous_) { glSetCurrentResourcePool(previous_); previous_ = nullptr; } }
};

// Last child of the original view graph: registered font packets follow world
// packets. Placement is diagnostic, not original textbox behavior.
class FrontendTextView final : public GLView
{
    FrontendFontRegistry& fonts_;
    resources::FrontendTextCatalog catalog_;
    resources::FontLayout display_;
    std::size_t selected_ = 0;
    unsigned rendered_ = 0;
    void Select(std::size_t index)
    {
        auto layout = catalog_.entries.at(index).layout;
        const float scale = std::min({1.f, 560.f / std::max(1.f, layout.width), 100.f / std::max(1.f, layout.height)});
        for (auto& quad : layout.quads)
        { quad.left *= scale; quad.right *= scale; quad.top *= scale; quad.bottom *= scale; }
        layout.width *= scale; layout.height *= scale;
        display_ = std::move(layout); selected_ = index;
    }
public:
    FrontendTextView(GLViewInterface& interface, FrontendFontRegistry& fonts, resources::FrontendTextCatalog catalog)
        : GLView(&interface, GLRenderPair{}, GLViewSort_None), fonts_(fonts), catalog_(std::move(catalog))
    {
        if (catalog_.entries.empty()) throw std::runtime_error("FEN has no text supported by the selected font/format profile");
        m_Name = "Frontend text inspection";
        const auto heading = std::find_if(catalog_.entries.begin(), catalog_.entries.end(), [](const auto& entry) {
            return entry.layout.font->alias == resources::FrontendNameHash("scratchy36");
        });
        Select(heading == catalog_.entries.end() ? 0 : std::size_t(heading - catalog_.entries.begin()));
    }
    void Step(bool previous)
    { Select(previous ? (selected_ + catalog_.entries.size() - 1) % catalog_.entries.size() : (selected_ + 1) % catalog_.entries.size()); }
    std::size_t Index() const { return selected_; }
    std::size_t Count() const { return catalog_.entries.size(); }
    unsigned Rendered() const { return rendered_; }
    const std::string& Name() const { return catalog_.entries[selected_].name; }
    void Submit()
    {
        nlMatrix4 model; model.SetIdentity(); model.e[12] = 40; model.e[13] = 350;
        fonts_.Submit(*this, display_, model);
    }
    void EndRender() override { ++rendered_; }
};

class FrontendFrameView final : public GLView
{
    FrontendPacketRenderer& packets_;
    unsigned rendered_ = 0;
public:
    FrontendFrameView(GLViewInterface& interface, FrontendPacketRenderer& packets)
        : GLView(&interface, GLRenderPair{}, GLViewSort_None), packets_(packets) {}
    unsigned Rendered() const { return rendered_; }
    std::size_t TextCount() const { return packets_.Current()->layout.TextCount(); }
    std::size_t ImageCount() const { return packets_.Current()->layout.ImageCount(); }
    void Submit(FrontendSession::Handle frame) { packets_.Submit(*this, std::move(frame)); }
    void EndRender() override { ++rendered_; }
};

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

int RunScenePreview(int argc, char** argv, const std::filesystem::path& config_path, const SceneOptions& requested)
{
    std::ofstream logfile;
    auto log = [&](const std::string& message) {
        std::cerr << "[scene] " << message << '\n';
        if (logfile) { logfile << "[scene] " << message << '\n'; logfile.flush(); }
    };
    try
    {
        auto options = requested;
        if (options.frontend_boot)
        {
            if (options.frontend_frame || options.frontend_layout || options.frontend_slide || options.frontend_images
                || options.frontend_animate || options.frontend_world || options.world || options.world_res
                || options.model_id || !options.object_ids.empty() || options.camera || options.debug_camera
                || options.nis_primary || options.nis_secondary || options.pip_expand || options.shadow_id
                || options.shadow_textures || options.particles || options.unlit || options.no_world_culling
                || options.model != SceneOptions{}.model || options.textures != SceneOptions{}.textures)
                throw std::invalid_argument("Retail boot selects its own scene and resources");
            options.frontend_frame = "/Art/fe/boot_loading.fen";
            options.frontend_images = "boot";
            options.frontend_animate = true;
        }
        if (options.nis_primary.has_value() != options.nis_secondary.has_value()
            || (options.pip_expand && !options.nis_primary)
            || (options.nis_primary && (options.camera || options.debug_camera || options.shadow_id || options.frontend_layout || options.frontend_frame)))
            throw std::invalid_argument("PIP requires two NIS paths and no other camera, shadow or text selection");
        if ((options.frontend_frame && options.frontend_layout) || (options.frontend_slide && !options.frontend_frame))
            throw std::invalid_argument("Select one frontend text mode; slide selection requires an authored frame");
        if (options.frontend_images && (!options.frontend_frame
            || (*options.frontend_images != "main" && *options.frontend_images != "ingame" && *options.frontend_images != "boot")))
            throw std::invalid_argument("Frontend images require an authored frame and main, ingame or boot context");
        if (options.frontend_animate && !options.frontend_frame)
            throw std::invalid_argument("Frontend animation requires an authored frame");
        if (options.particles && (options.nis_primary || options.shadow_id || options.shadow_textures))
            throw std::invalid_argument("Particle preview cannot be combined with PIP or shadow options");
        if (options.frontend_layout && options.debug_camera)
            throw std::invalid_argument("Frontend text inspection and debug camera use separate controls");
        if (options.frontend_world)
        {
            if (options.world || options.world_res || !options.object_ids.empty() || options.model_id || options.shadow_id)
                throw std::invalid_argument("Frontend world selection cannot be combined with explicit world/model/shadow IDs");
            if (!options.camera && !options.debug_camera && !options.nis_primary) options.camera = "/Art/fe/environments/cameras/start_idle.cam";
        }
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
        if (options.debug_camera && (options.camera || options.shadow_id))
            throw std::invalid_argument("Debug camera cannot be combined with authored camera or shadow preview");
        log(options.frontend_boot ? "Retail boot screen diagnostic. Complete game startup remains pending."
            : "Static material and stadium shadow preview. Full world loading, character animation and game scenes are pending.");
        const auto data_path = PathUtf8(directory);
        AuroraConfig config{};
        config.appName = options.frontend_boot ? "Mario Strikers Charged | Boot screen" : "Mario Strikers Charged | Static asset preview";
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
        GraphicsStartup graphics(GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight,
            [] { VIInit(); VIConfigure(&GXNtsc480IntDf); }, DrainGX);
        log("Original graphics memory initialized: two MEM1/MEM2 frames, Global resource pool, static GLInventory and 1000 texture indices.");
        log("Original GL state and identity matrix initialized; native frame matrix handles and original NL camera math enabled.");
        log(options.frontend_boot ? "Original InitializeCore completed; loading retail boot resources through original NL reads."
            : "Original InitializeCore completed; loading RLG/RLT through original NL whole-file async services.");
        const bool world_batch = options.world_res.has_value() || options.frontend_world;
        std::optional<resources::FrontendTextCatalog> frontend_text;
        std::vector<std::shared_ptr<const resources::FrontendFont>> inspector_fonts;
        std::optional<resources::FrontendLayoutFrame> frontend_frame;
        std::shared_ptr<FrontendSession> frontend_session;
        FrontendSession::Handle frontend_published_frame;
        EffectsRegistry::Handle particle_groups;
        std::unique_ptr<ParticleControllers> particles;
        const auto start_particles = [&] {
            // Complete source-audited group; every authored spec is required.
            // Two placements are explicit preview inputs, not actor bindings.
            for (float x : {-.6f, .6f})
            {
                const auto token = particles->Start(particle_groups, 0xe6650c7c);
                ParticleEmitterFrame frame; frame.position = {x, 0, 0};
                particles->SetFrame(token, frame);
            }
        };
        if (options.particles)
        {
            ParticleFileLoad load;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (load.State() == ParticleFileState::Loading)
            {
                if (Update()) throw std::runtime_error("Particle loading cancelled");
                load.Service();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Particle loading timed out");
                if (load.State() == ParticleFileState::Loading) SDL_Delay(1);
            }
            // Fixed source-audited USA group. An absent or unsupported record
            // is an error; selection never tries alternatives until one succeeds.
            particle_groups = EffectsRegistry::FromFiles(load.Result());
            particles = std::make_unique<ParticleControllers>();
            start_particles();
            unsigned systems = 0;
            for (const auto& controller : particles->Snapshot()) systems += controller.systems;
            log("Original particle group e6650c7c loaded through four NL reads: 2 controllers, "
                + std::to_string(systems) + " emitters; full effects-manager startup remains pending.");
        }
        const auto frontend_language = file.settings.language == "french" ? FrontendLanguage::NAFrench
            : file.settings.language == "spanish" ? FrontendLanguage::NASpanish : FrontendLanguage::English;
        if (options.frontend_frame)
        {
            frontend_session = std::make_shared<FrontendSession>();
            frontend_session->Begin({*options.frontend_frame, frontend_language,
                options.frontend_images == "boot" ? FrontendImageProfile::BootLoading
                    : options.frontend_images == "ingame" ? FrontendImageProfile::InGame : FrontendImageProfile::Main,
                options.frontend_slide.value_or(""), options.frontend_animate});
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (frontend_session->State() == FrontendSessionState::Loading)
            {
                if (Update()) throw std::runtime_error("Frontend scene loading cancelled");
                frontend_session->Service();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Frontend scene loading timed out");
                SDL_Delay(1);
            }
            const auto current = frontend_session->Result();
            frontend_published_frame = current;
            frontend_frame = current->layout;
            log("Frontend image context: " + options.frontend_images.value_or("main") + "; "
                + std::to_string(current->images->textures.size()) + " retained textures from "
                + std::to_string(current->image_completed_files) + " original bundle reads.");
            if (options.frontend_animate)
                log("Original frontend timeline selected; bounded runs advance at 60 Hz. Scene handlers and menu transitions remain pending.");
            log(std::string(options.frontend_animate ? "Authored frontend animated frame: " : "Authored frontend static frame: ")
                + std::to_string(frontend_frame->TextCount()) + " text components, "
                + std::to_string(frontend_frame->ImageCount()) + " image components; "
                + std::to_string(frontend_frame->hidden) + " hidden or inactive instances.");
            for (const auto& [reason, count] : frontend_frame->unavailable)
                log("Unavailable frontend frame components (" + std::to_string(count) + "): " + reason);
            if (frontend_frame->entries.empty() && !options.frontend_animate)
                throw std::runtime_error("Selected frontend frame has no supported static components");
            log("Frontend scene session owns FEN, fonts, images and playback; original handlers and menu actions remain pending.");
        }
        else if (options.frontend_layout)
        {
            FrontendVisualLoad visuals(frontend_language);
            PendingAsset layout; layout.Start(*options.frontend_layout);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!visuals.Ready() || !layout.done)
            {
                if (Update()) throw std::runtime_error("Frontend visual loading cancelled");
                nlServiceFileSystem(); visuals.Poll();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Frontend visual loading timed out");
                SDL_Delay(1);
            }
            const auto assets = visuals.Result();
            const auto graph = resources::ReadFrontendScene(layout.Bytes());
            const std::array fonts{assets->text, assets->heading};
            frontend_text = resources::InspectFrontendText(graph, *assets->localization, fonts);
            inspector_fonts.assign(fonts.begin(), fonts.end());
            log("Frontend layout decoded: " + std::to_string(graph.slides.size()) + " slides, "
                + std::to_string(graph.instances.size()) + " instances; " + std::to_string(frontend_text->entries.size())
                + " stored text components. Font/localization language: " + file.settings.language + ".");
            for (const auto& [reason, count] : frontend_text->unavailable)
                log("Unavailable text components (" + std::to_string(count) + "): " + reason);
            log("Text inspection uses original font pages/metrics and original FE input; authored layout, timelines and menu handlers remain pending.");
        }
        if (world_batch && !options.frontend_world && (!options.world || options.object_ids.empty() || options.model_id || options.shadow_id))
            throw std::invalid_argument("World objects require resident/temporary files and explicit object IDs only");
        if (options.world && !options.model_id && !world_batch) throw std::invalid_argument("World preview needs an explicit model ID");
        PendingAsset model_data, texture_data, resident_data;
        std::shared_ptr<const FrontendWorldFiles> frontend_files;
        if (options.frontend_world)
        {
            FrontendWorldFileLoad load;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (load.State() <= FrontendWorldFileState::Tweaks)
            {
                if (Update()) throw std::runtime_error("Frontend world loading cancelled");
                load.Service();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Frontend world loading timed out");
                SDL_Delay(1);
            }
            frontend_files = load.Result();
        }
        else if (!options.frontend_boot)
        {
            model_data.Start(options.world.value_or(options.model));
            if (!options.world) texture_data.Start(options.textures);
            if (world_batch) resident_data.Start(*options.world_res);
        }
        const auto load_start = std::chrono::steady_clock::now();
        while (!options.frontend_boot && !options.frontend_world && (!model_data.done || (!options.world && !texture_data.done) || (world_batch && !resident_data.done)))
        {
            if (Update()) throw std::runtime_error("Static preview cancelled while loading");
            nlServiceFileSystem();
            if (std::chrono::steady_clock::now() - load_start > std::chrono::seconds(30)) throw std::runtime_error("Static asset load timed out");
            SDL_Delay(1);
        }
        std::vector<resources::StaticModel> models;
        std::vector<resources::StaticWorldObject> world_objects;
        Bounds bounds;
        resources::TextureBundle texture_bundle;
        auto& textures = texture_bundle.textures;
        auto& animations = texture_bundle.animations;
        if (world_batch)
        {
            resources::StaticWorldScene scene;
            if (frontend_files)
            {
                auto available = resources::ReadAvailableWorldScene(frontend_files->resident, frontend_files->temporary);
                log("Frontend world coverage: " + std::to_string(available.scene.objects.size()) + " supported objects, "
                    + std::to_string(available.unavailable.size()) + " unavailable, " + std::to_string(available.parent_records)
                    + " parent records. Animation, effects, tweak application and menu state remain pending.");
                std::map<std::string, unsigned> missing;
                for (const auto& object : available.unavailable) ++missing[object.reason];
                for (const auto& [reason, count] : missing) log("Unavailable world objects (" + std::to_string(count) + "): " + reason);
                scene = std::move(available.scene); frontend_files.reset();
            }
            else
            {
                auto resident = resources::InflateAsset(resident_data.Bytes());
                auto temporary = resources::InflateAsset(model_data.Bytes());
                scene = resources::ReadStaticWorldScene(resident, temporary, options.object_ids);
            }
            models = std::move(scene.models); world_objects = std::move(scene.objects);
            texture_bundle = std::move(scene.textures); bounds = scene.bounds;
            log("Selected static world objects: " + std::to_string(world_objects.size()) + " instances, "
                + std::to_string(models.size()) + " shared models; original transforms retained.");
            log(options.no_world_culling ? "Diagnostic world culling disabled; all selected objects are submitted."
                : "Original sphere/box culling follows the current camera; the selected objects remain resident.");
        }
        else if (options.world)
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
        else if (!options.frontend_boot)
        {
            log(options.model + ": " + std::to_string(model_data.size) + " bytes; " + options.textures + ": " + std::to_string(texture_data.size) + " bytes.");
            models = resources::ReadStaticModels(model_data.Bytes(), options.model_id);
            texture_bundle = resources::ReadTextureBundle(texture_data.Bytes());
        }
        model_data.data.reset(); texture_data.data.reset(); resident_data.data.reset();
        auto selected = models.begin();
        if (options.model_id) selected = std::find_if(models.begin(), models.end(), [&](const auto& model) { return model.id == *options.model_id; });
        if (!options.frontend_boot && selected == models.end()) throw std::runtime_error("Requested model ID is absent from the RLG collection");
        if (!options.frontend_boot && !world_batch)
        {
            auto chosen = std::move(*selected); models.clear(); models.push_back(std::move(chosen));
        }
        auto* selected_model = models.empty() ? nullptr : &models.front();
        const auto selected_id = selected_model ? selected_model->id : 0;
        const auto shadow_packets = selected_model ? std::count_if(selected_model->packets.begin(), selected_model->packets.end(),
            [](const auto& p) { return p.material.program == 0x386ecbdd; }) : 0;
        const bool volume_preview = shadow_packets != 0;
        if (volume_preview && options.particles)
            throw std::invalid_argument("Particle preview is not connected to the diagnostic shadow receiver");
        if (volume_preview && shadow_packets != selected_model->packets.size())
            throw std::invalid_argument("Mixed shadow-volume and ordinary packets need original world selection");
        if (volume_preview && (options.unlit || options.shadow_id))
            throw std::invalid_argument("Object lighting and projected lookup options do not apply to shadow volumes");
        const bool camera_overlay = selected_model && std::any_of(selected_model->packets.begin(), selected_model->packets.end(),
            [](const auto& p) { return p.material.program == 0x32bc21e8 || p.material.program == 0x845cad59; });
        if (volume_preview && options.debug_camera)
            throw std::invalid_argument("Debug camera is not connected to the diagnostic shadow receiver");
        if (volume_preview && options.camera)
            throw std::invalid_argument("Authored camera playback is not connected to the diagnostic shadow receiver");
        if (volume_preview && options.nis_primary)
            throw std::invalid_argument("NIS PIP is not connected to the diagnostic shadow receiver");
        if (selected_model && !world_batch) bounds = Normalize(*selected_model, camera_overlay || options.camera.has_value() || options.debug_camera || options.nis_primary.has_value());
        std::size_t vertices = 0, indices = 0, packets = 0;
        std::vector<std::uint32_t> lookup_ids;
        for (const auto& model : models)
        {
            for (const auto id : MaterialLookupTextures(model))
                if (std::find(lookup_ids.begin(), lookup_ids.end(), id) == lookup_ids.end()) lookup_ids.push_back(id);
            for (const auto& packet : model.packets)
            {
                ++packets; vertices += packet.vertices.size(); indices += packet.indices.size();
                if (std::none_of(textures.begin(), textures.end(), [&](const auto& texture) { return texture.id == packet.material.textures[0].texture; })
                    && std::none_of(animations.begin(), animations.end(), [&](const auto& anim) { return anim.id == packet.material.textures[0].texture; }))
                    throw std::runtime_error("RLG diffuse texture is missing from the selected RLT bundle");
            }
        }
        std::ostringstream description;
        if (world_batch) description << "Selected world batch: ";
        else description << "Selected model 0x" << std::hex << selected_id << std::dec << ": ";
        description << packets << " packets, " << vertices << " vertices, " << indices << " indices; " << textures.size()
            << " textures, " << animations.size() << " texture animations; original radius " << bounds.radius << '.';
        if (!options.frontend_boot) log(description.str());
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
        // Validate camera files before GX queues commands for its first render target.
        OriginalCameras cameras;
        CameraPoseInput camera_input;
        std::optional<AnimatedCamera> authored_camera;
        std::optional<DebugCamera> debug_camera;
        std::optional<DebugCameraInput> debug_input;
        std::array<std::unique_ptr<NisCameraBinding>, 2> nis_bindings;
        std::unique_ptr<NisCameras> nis_cameras;
        std::unique_ptr<NisPlayback> nis_playback;
        std::unique_ptr<NisPip> pip;
        DebugCameraOrbit fitted_orbit;
        if (options.nis_primary)
        {
            std::array<PendingAsset, 2> reads;
            reads[0].Start(*options.nis_primary); reads[1].Start(*options.nis_secondary);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!reads[0].done || !reads[1].done)
            {
                if (Update()) throw std::runtime_error("NIS camera loading cancelled");
                nlServiceFileSystem();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("NIS camera loading timed out");
                SDL_Delay(1);
            }
            nis_cameras = std::make_unique<NisCameras>(cameras);
            for (unsigned slot = 0; slot < 2; ++slot)
            {
                NisCameraAssets assets(reads[slot].Bytes(), slot ? *options.nis_secondary : *options.nis_primary);
                nis_bindings[slot] = std::make_unique<NisCameraBinding>(assets, slot);
                nis_cameras->Select(slot, *nis_bindings[slot], 0);
            }
            nis_cameras->Activate();
            nis_playback = std::make_unique<NisPlayback>(*nis_cameras);
            pip = std::make_unique<NisPip>(*nis_playback, options.pip_expand.value_or(1.f));
            if (options.pip_expand) pip->SetMode(NisPipMode::Expand);
            log("Original NIS PIP: two retained authored cameras, 256x128 RGB565 target; static geometry only, no actors/audio/triggers.");
        }
        if (options.debug_camera)
        {
            // Keep original Z-up, nonnegative target height and world units.
            // The sphere fit covers the selected geometry even below Z=0.
            fitted_orbit.radius = std::max(.01f, 2.5f * bounds.radius + std::abs(std::min(0.f, bounds.center[2])));
            fitted_orbit.azimuth = -90; fitted_orbit.elevation = 35;
            fitted_orbit.height = std::max(0.f, bounds.center[2]);
            fitted_orbit.target_x = bounds.center[0]; fitted_orbit.target_y = bounds.center[1];
            debug_camera.emplace(); debug_camera->SetOrbit(fitted_orbit);
            debug_input.emplace();
            cCameraManager::PushCamera(&debug_camera->Camera());
            log("Original DebugCam: SDL keyboard/gamepad controls; model coordinates preserved. Diagnostic bindings only.");
        }
        if (options.camera)
        {
            CameraAssetLoad request(options.camera->c_str(), "preview");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!request.Ready())
            {
                if (Update()) throw std::runtime_error("Camera preview cancelled while loading");
                request.Service();
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Camera asset load timed out");
                if (!request.Ready()) SDL_Delay(1);
            }
            authored_camera.emplace(request.Result());
            cCameraManager::PushCamera(&authored_camera->Camera());
            log("Original authored camera playback: " + *options.camera + "; "
                + std::to_string(authored_camera->Duration()) + " seconds; model coordinates preserved. Depth-of-field rendering remains pending.");
        }
        else if (!debug_camera && !nis_cameras) cCameraManager::PushCamera(&camera_input);
        alignas(32) std::array<std::uint8_t, 65536> fifo{};
        std::unique_ptr<StaticInventory> inventory;
        std::unique_ptr<StaticWorldObjects> world;
        if (world_batch)
            world = std::make_unique<StaticWorldObjects>(world_objects, models, texture_bundle,
                WorldObjectMemory{4 * 1024 * 1024, 24 * 1024 * 1024}, DrainGX);
        else if (!options.frontend_boot) inventory = std::make_unique<StaticInventory>(*glGetCurrentResourcePool(), models, textures, DrainGX, animations);
        ScenePool pool_selection(world ? &world->Pool() : glGetCurrentResourcePool());
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
        if (!volume_preview && !options.frontend_boot)
            log(options.unlit ? "Unlit comparison selected." : "Original key/fill object-light defaults and ambient colour enabled; material lighting flags are preserved.");
        auto* native_model = inventory ? inventory->Model(selected_id) : nullptr;
        if (!options.frontend_boot && !world && !native_model) throw std::runtime_error("Selected model is missing from original GLInventory");
        if (!options.frontend_boot)
            log("Checked RLG/RLT data installed as pool-owned native glModel/PlatTexture records; drawing original material Activate/Draw/Deactivate and TEV shader recipes through Aurora.");
        ViewMatrices view_matrices;
        AuroraFrames backend;
        OriginalFrames lifecycle(backend);
        FrameCounter timing("frame", "send");
        GLView* submitted = nullptr;
        GLView* world_alpha = nullptr;
        RLViewCamera shadow_camera;
        std::unique_ptr<ShadowLayers> shadow_layers;
        std::unique_ptr<StadiumShadowVolume> shadow_drawable;
        std::unique_ptr<NisPipScene> pip_scene;
        if (pip) pip_scene = std::make_unique<NisPipScene>();
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
            child->m_Name = world ? "World opaque" : "Static model";
            if (pip) child->m_ClearColour = child->m_ClearDepth = true;
            if (world)
            {
                // Children render first: opaque child, then its alpha parent.
                auto alpha = std::make_unique<GLView>(&view_matrices, GLRenderPair{}, GLViewSort_Texture);
                alpha->m_Name = "World alpha";
                alpha->AddChild(child.get()); submitted = child.release();
                gRootView.AddChild(alpha.get()); world_alpha = alpha.release();
            }
            else
            {
                gRootView.AddChild(child.get()); submitted = child.release();
            }
        }
        if (pip_scene) pip_scene->AttachOverlay();
        GLView* particle_view = nullptr;
        std::unique_ptr<GLResourcePool, void (*)(GLResourcePool*)> particle_pool{nullptr, glDestroyResourcePool};
        std::unique_ptr<ParticleControllerRenderer> particle_renderer;
        if (particles)
        {
            // Resetting these bindings must not rewind the world/font owners.
            const GLMemoryRequirement requirement{GLM_Header, 128 * 1024};
            particle_pool.reset(glCreateResourcePool(&requirement, 1, "Particle controller textures"));
            particle_renderer = std::make_unique<ParticleControllerRenderer>(*particle_pool, *particles, DrainGX);
            auto view = std::make_unique<GLView>(&view_matrices, GLRenderPair{}, GLViewSort_None);
            view->m_Name = "Original billboard particles";
            gRootView.AddChild(view.get()); particle_view = view.release();
            log("Particle controller textures registered in the native GL pool: "
                + std::to_string(particle_renderer->Textures()) + " shared bindings; original billboard meshes follow world packets.");
        }
        log("Original GLView graph, packet sorting and callback flags connected to Aurora; native target registry initialized.");
        FrontendTextView* text_view = nullptr;
        ViewMatrices text_matrices;
        std::unique_ptr<FrontendFontRegistry> font_registry;
        FrontendFrameView* frame_view = nullptr;
        std::unique_ptr<FrontendPacketRenderer> frame_packets;
        std::unique_ptr<FrontendInput> frontend_input;
        std::unique_ptr<FrontendInputSDL> frontend_devices;
        std::unique_ptr<FrontendHandler> frontend_handler;
        std::unique_ptr<FrontendBootLoading> frontend_boot;
        if (frontend_text)
        {
            font_registry = std::make_unique<FrontendFontRegistry>(*glGetCurrentResourcePool(), inspector_fonts, DrainGX);
            glMatrixOrthographic(text_matrices.projection, GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight);
            frontend_input = std::make_unique<FrontendInput>();
            frontend_devices = std::make_unique<FrontendInputSDL>();
            frontend_input->EnableAnalogDirections(true);
            for (const auto action : {FrontendAction::Up, FrontendAction::Down})
                frontend_input->SetRepeat(action, .35f, .12f);
            auto view = std::make_unique<FrontendTextView>(text_matrices, *font_registry, std::move(*frontend_text));
            gRootView.AddChild(view.get()); text_view = view.release(); frontend_text.reset();
            inspector_fonts.clear();
            log("Frontend text uses registered font pages and original GL packets.");
        }
        if (frontend_frame)
        {
            frame_packets = std::make_unique<FrontendPacketRenderer>(DrainGX);
            frontend_input = std::make_unique<FrontendInput>();
            frontend_devices = std::make_unique<FrontendInputSDL>();
            frontend_input->EnableAnalogDirections(true);
            for (const auto action : {FrontendAction::Left, FrontendAction::Right})
                frontend_input->SetRepeat(action, .35f, .12f);
            if (options.frontend_boot)
            {
                frontend_boot = std::make_unique<FrontendBootLoading>(frontend_session, *frontend_input);
                frontend_published_frame = frontend_boot->Current();
                log("Original retail BootLoadingScene selected: strap, nunchuk and ESRB; audio service remains pending.");
            }
            else frontend_handler = std::make_unique<FrontendHandler>(frontend_session, *frontend_input);
            frame_packets->Prepare(frontend_session->Current());
            glMatrixOrthographic(text_matrices.projection, GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight);
            auto view = std::make_unique<FrontendFrameView>(text_matrices, *frame_packets);
            view->m_Name = options.frontend_animate ? "Authored frontend animated layout" : "Authored frontend static layout";
            if (options.frontend_boot)
            {
                // Original glxInitTargets and glxSwap clear the boot backbuffer
                // to transparent black. The diagnostic frame owns that clear
                // until the complete original swap loop is connected.
                glGetBackBufferTarget().target->mClearColour = {0, 0, 0, 0};
                view->m_ClearColour = view->m_ClearDepth = true;
            }
            gRootView.AddChild(view.get()); frame_view = view.release(); frontend_frame.reset();
            log("Mixed frontend text and images use retained registrations and original ordered GL packets.");
        }
        log("Original graphics begin/end/send lifecycle connected; host work drains before frame memory reuse.");
        if (!options.frontend_boot) log("Original camera core supplies the view and position; full gameplay camera selection remains pending.");
        bool volume_enabled = true;
        float receiver_height = 0;
        // Render only game-pool records from here; discard host decoder storage.
        models.clear(); models.shrink_to_fit(); textures.clear(); textures.shrink_to_fit();
        animations.clear(); animations.shrink_to_fit();
        world_objects.clear(); world_objects.shrink_to_fit();
        unsigned frames = 0, draws = 0, depth_hits = 0, colour_hits = 0, shadow_hits = 0;
        bool world_culling = !options.no_world_culling;
        bool animation_paused = false, animation_reset = false;
        unsigned animation_updates = 0;
        bool particles_paused = false, particles_reset = false, particles_visible = true;
        unsigned particle_updates = 0, particle_peak = 0, particle_live = 0, particle_active_controllers = 0;
        std::size_t particle_submissions = 0;
        bool preserve_component_time = false;
        std::array<char, 256> frontend_instance_path{};
        std::string frontend_message;
        FrontendSession::Handle frontend_failed_graphics;
        std::optional<std::chrono::steady_clock::time_point> frontend_deadline;
        const auto frontend_action = [&](auto&& action) {
            try { action(); frontend_message.clear(); }
            catch (const std::exception& error) {
                frontend_message = error.what();
                log("Frontend session: " + frontend_message + "; current scene retained.");
            }
        };
        StaticWorldSubmission world_submission;
        std::size_t world_considered = 0, world_visible = 0, world_packets = 0;
        std::size_t pip_objects = 0, pip_packets = 0;
        const auto start = std::chrono::steady_clock::now();
        float elapsed = 0, delta = 0;
        GraphicsFrameTasks frame_tasks(graphics, lifecycle, {
            [&](float) { timing.StartTimer(0); },
            [&](float scheduler_delta) {
                elapsed = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                delta = scheduler_delta;
                glGetCurrentResourcePool()->m_inventory->UpdateTextureAnims(delta);
                if (particles)
                {
                    if (!particles_paused)
                    {
                        particles->Advance(options.frames ? 1.f / 60 : std::clamp(delta, 0.f, .1f));
                        ++particle_updates;
                    }
                    const auto snapshot = particles->Snapshot();
                    particle_active_controllers = static_cast<unsigned>(snapshot.size());
                    particle_live = 0;
                    for (const auto& controller : snapshot) particle_live += controller.particles;
                    particle_peak = std::max(particle_peak, particle_live);
                }
                if (frontend_input)
                {
                    const auto& io = ImGui::GetIO();
                    // Bounded boot diagnostics qualify the auto-dismiss path
                    // with explicit neutral input, independently of the desktop.
                    if (frontend_boot && options.frames) frontend_input->Update({}, 1.f / 60);
                    else frontend_devices->Poll(*frontend_input, info.window, std::clamp(delta, 0.f, .1f),
                        io.WantCaptureKeyboard, io.NavActive && (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad));
                    if (text_view)
                    {
                        const auto previous = text_view->Index();
                        if (frontend_input->Button(FrontendAction::Up, FrontendButtonQuery::Repeat)) text_view->Step(true);
                        else if (frontend_input->Button(FrontendAction::Down, FrontendButtonQuery::Repeat)) text_view->Step(false);
                        if (previous != text_view->Index())
                            log("Original FE input selected text component " + std::to_string(text_view->Index() + 1) + ".");
                    }
                }
                const bool frontend_ready = frontend_session && frame_packets->Current() == frontend_session->Current();
                if (frontend_ready && options.frontend_animate)
                {
                    if (animation_reset)
                    {
                        if (frontend_boot)
                        {
                            frontend_boot->Reset(frame_packets->Current());
                            log("Retail boot screen reset by preview control.");
                        }
                        else frontend_session->Reset();
                        animation_reset = false;
                    }
                    else if (!animation_paused)
                    {
                        const float step = frames ? (options.frames ? 1.f / 60 : std::clamp(delta, 0.f, .1f)) : 0.f;
                        if (frontend_boot)
                        {
                            const auto before = frontend_boot->Status();
                            frontend_boot->Update(frame_packets->Current(), step);
                            const auto after = frontend_boot->Status();
                            if (before.phase != after.phase)
                                log("Original boot screen phase: " + std::to_string(after.phase));
                            if (before.boundary != after.boundary)
                                log("Boot screen stopped at FEAudio::PlaySound(0x17, 0xde83984e). Audio and full startup remain pending.");
                        }
                        else frontend_handler->Update(frame_packets->Current(), step);
                        ++animation_updates;
                    }
                    // Diagnostic authored-slide selection, never a concrete
                    // game's menu action. DebugCam owns arrow/stick controls.
                    if (!debug_camera && !frontend_boot)
                    {
                        const auto current = frontend_handler->Current();
                        const bool previous = frontend_handler->Button(current, FrontendAction::Left, FrontendButtonQuery::Repeat);
                        const bool next = frontend_handler->Button(current, FrontendAction::Right, FrontendButtonQuery::Repeat);
                        const auto& slides = current->graph.presentation_slides;
                        if ((previous || next) && !slides.empty()) frontend_action([&] {
                            const auto active = std::find(slides.begin(), slides.end(), current->graph.active_slide);
                            const std::size_t index = active == slides.end() ? 0 : std::size_t(active - slides.begin());
                            const auto selected = slides[(index + (previous ? slides.size() - 1 : 1)) % slides.size()];
                            const auto slide = std::find_if(current->graph.slides.begin(), current->graph.slides.end(),
                                [&](const auto& value) { return value.offset == selected; });
                            if (slide == current->graph.slides.end()) throw std::logic_error("Authored presentation slide is missing");
                            frontend_session->SelectPresentation(slide->name);
                            log("Original FE input selected presentation slide: " + slide->name + ".");
                        });
                    }
                }
                if (frontend_ready) frontend_published_frame = frontend_session->Current();

            },
            [&](float) {
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
                auto* ground=inventory->Model(receiver_id); glModelSetMatrix(ground, receiver);
                shadow_layers->Layer(eCLV_Shadowed).AttachModel(ground, 0);
                if (volume_enabled) shadow_drawable->Draw(identity);
                RenderShadowVolumeBlend(&shadow_layers->Layer(eCLV_ShadowVolumeBlend));
            }
            else if (nis_cameras)
            {
                const auto step = nis_playback->Advance({frames ? (options.frames ? 1.f/60 : delta) : 0.f, 1, 0x10});
                if (step.active) pip->Update(step.delta);
                cameras.Advance(0,0);
                view_matrices.view = cCameraManager::m_matView;
                glMatrixPerspective(view_matrices.projection, cCameraManager::m_fFOV * 3.1415927f / 180, 4.f/3, .25f, 4096.f);
                backend.camera_position = cCameraManager::m_cameraPosition;
                pip_scene->SetCamera(*nis_cameras->Camera(1));
                if (world)
                {
                    const auto& matrices = pip_scene->Matrices();
                    const auto secondary = SubmitStaticWorld(*world, pip_scene->Opaque(), pip_scene->Alpha(),
                        StaticWorldFrustum::FromCamera(matrices.view, matrices.projection), world_culling);
                    pip_objects += secondary.visible;
                    pip_packets += secondary.opaque_packets + secondary.alpha_packets;
                }
                else
                {
                    nlMatrix4 identity; identity.SetIdentity(); glModelSetMatrix(native_model, identity);
                    pip_scene->Opaque().AttachModel(native_model, 0); submitted->AttachModel(native_model, 0);
                }
                pip_scene->Submit(*pip);
            }
            else if (authored_camera)
            {
                // Bounded diagnostics use a reproducible 60 Hz playback clock.
                const float camera_delta = frames ? (options.frames ? 1.f / 60 : delta) : 0;
                cameras.Advance(camera_delta, camera_delta);
                GXSetCopyClear({24,28,34,255}, GX_MAX_Z24);
                view_matrices.view = cCameraManager::m_matView;
                glMatrixPerspective(view_matrices.projection, cCameraManager::m_fFOV * 3.1415927f / 180,
                    float(GXNtsc480IntDf.fbWidth) / GXNtsc480IntDf.efbHeight, .1f, 1000.f);
                if (!world)
                {
                    nlMatrix4 identity; identity.SetIdentity(); glModelSetMatrix(native_model, identity);
                    submitted->AttachModel(native_model, 0);
                }
                backend.camera_position = cCameraManager::m_cameraPosition;
            }
            else if (debug_camera)
            {
                const auto& io = ImGui::GetIO();
                const bool gamepad_capture = io.NavActive && (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad);
                const auto command = debug_input->Poll(info.window, io.WantCaptureKeyboard, gamepad_capture);
                debug_camera->SetInputs(command.inputs);
                if (command.reset) debug_camera->SetOrbit(fitted_orbit);
                // Avoid applying time spent unfocused or stalled as one movement.
                const float camera_delta = frames ? (options.frames ? 1.f/60 : std::clamp(delta, 0.f, .1f)) : 0;
                cameras.Advance(camera_delta, camera_delta);
                view_matrices.view = cCameraManager::m_matView;
                const float far_plane = std::max(100.f, 20 * (bounds.radius + debug_camera->Orbit().radius));
                glMatrixPerspective(view_matrices.projection, cCameraManager::m_fFOV * 3.1415927f / 180,
                    float(GXNtsc480IntDf.fbWidth) / GXNtsc480IntDf.efbHeight, std::max(.001f, far_plane / 200000.f), far_plane);
                GXSetCopyClear({24,28,34,255}, GX_MAX_Z24);
                if (!world)
                {
                    nlMatrix4 identity; identity.SetIdentity(); glModelSetMatrix(native_model, identity);
                    submitted->AttachModel(native_model, 0);
                }
                backend.camera_position = cCameraManager::m_cameraPosition;
            }
            else if (world)
            {
                // Diagnostic Z-up orbit around transformed world bounds.
                const auto& c = bounds.center; const float radius = bounds.radius;
                camera_input.position = {c[0] + 3 * radius * std::sin(elapsed * .15f),
                    c[1] - 3 * radius * std::cos(elapsed * .15f), c[2] + 1.5f * radius};
                camera_input.target = {c[0], c[1], c[2]};
                glMatrixLookAt(camera_input.view, camera_input.position, camera_input.target, {0,0,1});
                cameras.Advance(0,0); view_matrices.view = cCameraManager::m_matView;
                glMatrixPerspective(view_matrices.projection, cCameraManager::m_fFOV * 3.1415927f / 180,
                    float(GXNtsc480IntDf.fbWidth) / GXNtsc480IntDf.efbHeight, .01f * radius, 20 * radius);
                GXSetCopyClear({24,28,34,255}, GX_MAX_Z24);
                backend.camera_position = cCameraManager::m_cameraPosition;
            }
            else if (native_model) backend.camera_position = SubmitModel(*native_model, *submitted, view_matrices, cameras, camera_input, elapsed,
                                                       camera_overlay ? &bounds : nullptr);
            if (world)
            {
                const auto frustum = StaticWorldFrustum::FromCamera(view_matrices.view, view_matrices.projection);
                world_submission = SubmitStaticWorld(*world, *submitted, *world_alpha, frustum, world_culling);
                world_considered += world_submission.objects;
                world_visible += world_submission.visible;
                world_packets += world_submission.opaque_packets + world_submission.alpha_packets;
            }
            if (particle_renderer)
                particle_submissions += particle_renderer->Submit(*particle_view, particles_visible);
            if (text_view) text_view->Submit();
            if (frame_view) frame_view->Submit(frontend_published_frame);
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
            ImGui::Begin(options.frontend_boot ? "Retail boot screen" : "Static asset preview", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            if (frontend_boot)
            {
                const auto state = frontend_boot->Status();
                ImGui::TextUnformatted("Original startup screen / work in progress");
                if (state.boundary == FrontendBootBoundary::PlayLogoSound)
                    ImGui::TextUnformatted("Stopped: logo sound playback is not implemented yet.");
                else ImGui::TextUnformatted("Enter or controller A: continue after the original delay.");
                ImGui::BeginDisabled(options.frames != 0);
                if (ImGui::Button("Restart boot screen")) animation_reset = true;
                ImGui::EndDisabled();
                if (frame_packets->Current() != frontend_session->Current())
                {
                    ImGui::TextUnformatted("Graphics replacement failed; the previous frame remains visible.");
                    if (ImGui::Button("Retry frontend graphics")) frontend_failed_graphics.reset();
                }
                if (!frontend_message.empty()) ImGui::TextWrapped("Frontend: %s", frontend_message.c_str());
            }
            else ImGui::TextUnformatted("Original Wii mesh and material programs / Aurora GX Vulkan");
            if (particles)
            {
                ImGui::Text("Original particles: %u live | %u controllers", particle_live, particle_active_controllers);
                ImGui::Checkbox("Pause particles", &particles_paused);
                ImGui::SameLine(); ImGui::Checkbox("Show particles", &particles_visible);
                if (ImGui::Button("Reset particles")) particles_reset = true;
                ImGui::TextUnformatted("Two group instances; complete effects manager pending.");
            }
            if (text_view)
            {
                ImGui::Text("Stored FE text: %zu / %zu | %s", text_view->Index() + 1, text_view->Count(), text_view->Name().c_str());
                ImGui::TextUnformatted("Up/Down or pad: inspect text | Font rendering only; original menu/layout pending.");
                if (ImGui::Button("Previous text")) text_view->Step(true);
                ImGui::SameLine(); if (ImGui::Button("Next text")) text_view->Step(false);
            }
            if (frame_view && !frontend_boot)
            {
                ImGui::Text("Authored %s layout: %zu text, %zu images", options.frontend_animate ? "animated" : "static",
                    frame_view->TextCount(), frame_view->ImageCount());
                const bool editable = frame_packets->Current() == frontend_session->Current();
                ImGui::BeginDisabled(!editable);
                if (frontend_session && options.frontend_animate)
                {
                    ImGui::Text("Frontend time: %.2f s", frame_packets->Current()->graph.presentation_time);
                    ImGui::Checkbox("Pause frontend", &animation_paused);
                    ImGui::SameLine(); if (ImGui::Button("Reset frontend")) animation_reset = true;
                    const auto current = frame_packets->Current();
                    const auto& graph = current->graph;
                    const auto slide_name = [&](resources::FrontendReference id) -> const char* {
                        for (const auto& slide : graph.slides) if (id == slide.offset) return slide.name.c_str();
                        return "None";
                    };
                    if (ImGui::TreeNode("Slide selection"))
                    {
                        ImGui::SetNextItemWidth(190);
                        if (ImGui::BeginCombo("Presentation", slide_name(graph.active_slide)))
                        {
                            for (auto id : graph.presentation_slides)
                                if (ImGui::Selectable(slide_name(id), id == graph.active_slide))
                                    frontend_action([&] { frontend_session->SelectPresentation(slide_name(id)); });
                            ImGui::EndCombo();
                        }
                        ImGui::Checkbox("Preserve component time", &preserve_component_time);
                        ImGui::BeginChild("Component slides", {430, 140}, ImGuiChildFlags_Borders);
                        for (const auto& component : graph.library)
                        {
                            if (component.type != 3 || component.slides.empty()) continue;
                            ImGui::PushID(static_cast<int>(component.offset));
                            ImGui::SetNextItemWidth(190);
                            if (ImGui::BeginCombo(component.name.c_str(), slide_name(component.active_slide)))
                            {
                                for (auto id : component.slides)
                                    if (ImGui::Selectable(slide_name(id), id == component.active_slide))
                                        frontend_action([&] { frontend_session->SelectComponent(component.offset,
                                            slide_name(id), false, preserve_component_time); });
                                ImGui::EndCombo();
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndChild();
                        ImGui::TreePop();
                    }
                    ImGui::TextUnformatted("Original base update; game menus and transitions pending.");
                    ImGui::TextUnformatted(debug_camera ? "Slide shortcuts disabled while DebugCam controls are active."
                        : "Left/Right or pad: inspect authored presentation slides.");
                }
                else ImGui::TextUnformatted("Stored frame only; use --frontend-animate for a timeline.");
                if (ImGui::TreeNode("Instance inspection"))
                {
                    ImGui::SetNextItemWidth(330);
                    ImGui::InputTextWithHint("Path", "Layer/Item", frontend_instance_path.data(), frontend_instance_path.size());
                    const auto current = frame_packets->Current();
                    std::optional<resources::FrontendNode> node;
                    std::string lookup_error;
                    try
                    {
                        const std::string_view path(frontend_instance_path.data());
                        if (!path.empty())
                        {
                            std::vector<std::string_view> names;
                            for (std::size_t begin = 0;;)
                            {
                                const auto end = path.find('/', begin);
                                const auto name = path.substr(begin, end == path.npos ? path.size() - begin : end - begin);
                                if (name.empty()) throw std::invalid_argument("Path components must not be empty");
                                names.push_back(name);
                                if (end == path.npos) break;
                                begin = end + 1;
                            }
                            node = resources::FindFrontendNode(current->graph, {}, resources::FrontendNamedPath(names));
                            if (!node) lookup_error = "No matching component in the active scene";
                        }
                    }
                    catch (const std::exception& error) { lookup_error = error.what(); }
                    if (node && node->kind == resources::FrontendNodeKind::Instance)
                    {
                        const auto& graph = current->graph;
                        const auto found = std::find_if(graph.instances.begin(), graph.instances.end(),
                            [&](const auto& instance) { return instance.offset == node->id; });
                        if (found == graph.instances.end()) throw std::logic_error("Frontend finder returned an absent instance");
                        const auto library = std::find_if(graph.library.begin(), graph.library.end(),
                            [&](const auto& value) { return found->library == value.offset; });
                        auto position = found->attributes.position;
                        auto colour = found->attributes.colour;
                        if (library != graph.library.end())
                        {
                            if (!(found->overload_flags & 1)) position = library->attributes.position;
                            if (!(found->overload_flags & 16)) colour = library->attributes.colour;
                        }
                        std::vector<resources::FrontendInstanceChange> changes;
                        bool visible = found->visible;
                        if (ImGui::Checkbox("Visible", &visible))
                        {
                            resources::FrontendInstanceChange change;
                            change.instance = node->id; change.property = resources::FrontendInstanceProperty::Visible;
                            change.flag = visible; changes.push_back(std::move(change));
                        }
                        ImGui::SetNextItemWidth(330);
                        if (ImGui::DragFloat3("Position", position.data(), 1, -10000, 10000, "%.1f"))
                        {
                            resources::FrontendInstanceChange change;
                            change.instance = node->id; change.property = resources::FrontendInstanceProperty::Position;
                            change.vector = position; changes.push_back(std::move(change));
                        }
                        std::array<float,4> rgba{};
                        for (unsigned i = 0; i < 4; ++i) rgba[i] = colour[i] / 255.f;
                        ImGui::SetNextItemWidth(330);
                        if (ImGui::ColorEdit4("Colour", rgba.data()))
                        {
                            if (std::all_of(rgba.begin(), rgba.end(), [](float value) { return std::isfinite(value); }))
                            {
                                resources::FrontendInstanceChange change;
                                change.instance = node->id; change.property = resources::FrontendInstanceProperty::Colour;
                                for (unsigned i = 0; i < 4; ++i)
                                    change.colour[i] = static_cast<std::uint8_t>(std::clamp(rgba[i], 0.f, 1.f) * 255.f + .5f);
                                changes.push_back(std::move(change));
                            }
                            else frontend_message = "Colour components must be finite";
                        }
                        if (!changes.empty()) frontend_action([&] { frontend_session->Apply(current, changes); });
                        ImGui::TextUnformatted("Preview edits; authored animation may update these values. Reload restores the file.");
                    }
                    else if (node) ImGui::TextUnformatted("The path selects a slide; choose one of its component instances.");
                    if (!lookup_error.empty()) ImGui::TextWrapped("%s", lookup_error.c_str());
                    ImGui::TreePop();
                }
                ImGui::EndDisabled();
                if (!editable)
                {
                    ImGui::TextUnformatted("Graphics replacement failed; the previous frame remains visible.");
                    if (ImGui::Button("Retry frontend graphics")) frontend_failed_graphics.reset();
                }
                if (frontend_session->State() == FrontendSessionState::Loading)
                {
                    ImGui::TextUnformatted("Reloading frontend; current scene remains active.");
                    if (ImGui::Button("Cancel reload")) frontend_action([&] {
                        frontend_session->Cancel(); frontend_deadline.reset();
                    });
                }
                else if (ImGui::Button("Reload frontend")) frontend_action([&] {
                    frontend_session->Begin(frame_packets->Current()->request);
                    frontend_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                });
                if (!frontend_message.empty()) ImGui::TextWrapped("Frontend: %s", frontend_message.c_str());
            }
            if (world)
            {
                ImGui::Text("Static world selection: %zu / %zu objects submitted", world_submission.visible, world_submission.objects);
                ImGui::Checkbox("Frustum culling", &world_culling);
            }
            if (authored_camera)
                ImGui::Text("Authored camera: %.2f / %.2f s", authored_camera->Time() * authored_camera->Duration(), authored_camera->Duration());
            if (debug_camera)
            {
                ImGui::Text("Original DebugCam | %s | %s", debug_camera->ControlsEnabled() ? "controls enabled" : "controls frozen",
                    debug_input->GamepadConnected() ? "gamepad connected" : "keyboard");
                ImGui::TextUnformatted("Arrows: orbit | WASD: pan | Q/E: radius | Shift+Q/E: height");
                ImGui::TextUnformatted("Pad: sticks orbit/pan | LB/RB radius | X+LB/RB or LT/RT height");
                ImGui::TextUnformatted("Q+E / LB+RB: toggle controls | R / Back: reset pose");
                if (ImGui::Button("Reset camera pose")) debug_camera->SetOrbit(fitted_orbit);
                const auto orbit = debug_camera->Orbit();
                ImGui::SameLine(); ImGui::Text("Radius %.2f | height %.2f", orbit.radius, orbit.height);
                ImGui::TextUnformatted("Click the scene to release UI input. Focus/capture changes require neutral controls.");
            }
            if (volume_preview)
            {
                ImGui::TextUnformatted("Original stadium shadow mesh / diagnostic receiver");
                ImGui::Checkbox("Shadow volume", &volume_enabled);
                ImGui::SliderFloat("Receiver height", &receiver_height, -.8f, .8f);
            }
            else if (!frontend_boot) ImGui::Checkbox("Object lighting", &lighting.enabled);
            if (options.shadow_id)
            {
                ImGui::Checkbox("Projected shadow lookup", &shadows_enabled);
                ImGui::SliderFloat2("Shadow scale", lighting.shadow.scale.data(), 0.001f, 2.0f);
                ImGui::SliderFloat2("Shadow offset", lighting.shadow.translation.data(), -1, 1);
            }
            if (pip)
            {
                if (ImGui::Button("PIP")) pip->SetMode(NisPipMode::Pip);
                ImGui::SameLine(); if (ImGui::Button("Swap cameras")) pip->SetMode(NisPipMode::Swap);
                ImGui::SameLine(); if (ImGui::Button("Expand PIP")) pip->SetMode(NisPipMode::Expand);
            }
            if (!frontend_boot) ImGui::TextUnformatted("Full stadium scenes and character animation are pending.");
            ImGui::TextUnformatted("Escape or close the window to exit."); ImGui::End();
            backend.read_colours = (!colour_hits && frames % 15 == 14) || (options.frames && frames + 1 == options.frames);
            },
            [&](float) { timing.StartTimer(1); }
        });
        const auto finish_renderers = [&] {
            std::exception_ptr failure;
            try { if (font_registry) font_registry->FinishFrame(); }
            catch (...) { failure = std::current_exception(); }
            try { if (frame_packets) frame_packets->FinishFrame(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
            try { if (particle_renderer) particle_renderer->FinishFrame(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
            if (failure) std::rethrow_exception(failure);
        };
        log("Selected begin/update/render/end callbacks use original nlTaskManager priorities 4/9/11/16; full game tasks remain pending.");
        while (!options.frames || frames < options.frames)
        {
            if (Update()) break;
            nlServiceFileSystem();
            if (frontend_session && frontend_session->State() == FrontendSessionState::Loading)
            {
                frontend_action([&] {
                    frontend_session->Service();
                    if (frontend_session->State() == FrontendSessionState::Loading)
                    {
                        if (frontend_deadline && std::chrono::steady_clock::now() > *frontend_deadline)
                        {
                            frontend_session->Cancel();
                            throw std::runtime_error("Frontend reload timed out");
                        }
                    }
                    else
                    {
                        frontend_deadline.reset();
                        frontend_session->Result();
                        log("Frontend replacement published after all selected resources completed.");
                    }
                });
            }
            if (frontend_session)
            {
                const auto next = frontend_session->Current();
                if (next != frame_packets->Current() && next != frontend_failed_graphics)
                {
                    try { frame_packets->Prepare(next); frontend_failed_graphics.reset(); frontend_message.clear(); }
                    catch (const std::exception& error)
                    {
                        frontend_failed_graphics = next; frontend_message = error.what();
                        log("Frontend graphics replacement failed: " + frontend_message + "; previous frame retained.");
                    }
                }
                frontend_published_frame = frame_packets->Current();
            }
            if (particles_reset)
            {
                // The previous frame is drained. Recreate bindings only while
                // idle, after invalidating the old controller tokens.
                particle_renderer.reset();
                particles->Reset(0x9184eb0c);
                start_particles();
                particle_renderer = std::make_unique<ParticleControllerRenderer>(*particle_pool, *particles, DrainGX);
                particles_reset = false;
            }
            if (options.frames && std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) throw std::runtime_error("Static preview frame deadline exceeded");
            if (!lifecycle.Acquire()) { SDL_Delay(1); continue; }
            if (!session.gx)
            {
                // GXInit queues viewport state; its first target must exist
                // before any exception path can drain those commands.
                GXInit(fifo.data(), fifo.size()); session.gx = true;
                SetGraphicsCacheInvalidator(InvalidateCaches);
                AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
            }
            try { frame_tasks.RunAcquired(); }
            catch (...)
            {
                // The frame owner cancels/drains before propagating failures.
                finish_renderers();
                throw;
            }
            finish_renderers();
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
        frame_tasks.Release();
        if (pip && world) log("NIS secondary world submission totals: " + std::to_string(pip_objects)
            + " visible objects, " + std::to_string(pip_packets) + " packets. Pixel visibility is camera-dependent.");
        pip_scene.reset(); pip.reset(); nis_playback.reset(); nis_cameras.reset();
        for (auto& binding : nis_bindings) binding.reset();
        if (frame_view)
        {
            if (frame_view->Rendered() != frames) throw std::runtime_error("Frontend frame missed a render pass");
            log("Authored frontend frame rendered: " + std::to_string(frames) + " frames, "
                + std::to_string(frame_view->TextCount()) + " text components, "
                + std::to_string(frame_view->ImageCount())
                + (options.frontend_animate ? " image components in the last frame." : " image components per frame."));
            if (frontend_session && options.frontend_animate) log("Original frontend timeline advanced: " + std::to_string(animation_updates)
                + " updates; presentation time " + std::to_string(frontend_session->Current()->graph.presentation_time) + " seconds.");
            if (frontend_boot)
            {
                const auto status = frontend_boot->Status();
                log("Retail boot final phase: " + std::to_string(status.phase) + "; elapsed "
                    + std::to_string(status.elapsed) + "; strap alpha " + std::to_string(status.strap_alpha));
            }
        }
        if (text_view)
        {
            if (text_view->Rendered() != frames) throw std::runtime_error("Frontend text view missed a rendered frame");
            log("Frontend text inspection rendered: " + std::to_string(text_view->Rendered()) + " frames through the original view graph.");
        }
        frontend_boot.reset(); frontend_handler.reset(); frontend_devices.reset(); frontend_input.reset();
        frontend_session.reset(); // Drain pending reloads before NL services/arenas shut down.
        frontend_published_frame.reset();
        if (particles)
            log("Original particle preview rendered: " + std::to_string(particle_updates) + " updates, "
                + std::to_string(particle_submissions) + " submitted quads, " + std::to_string(particle_peak) + " peak live.");
        frame_packets.reset(); frontend_failed_graphics.reset();
        font_registry.reset();
        particle_renderer.reset(); particle_pool.reset(); particles.reset(); particle_groups.reset();
        debug_input.reset(); debug_camera.reset(); authored_camera.reset(); cameras.Release();
        lifecycle.Release(); shadow_drawable.reset(); shadow_layers.reset(); pool_selection.Restore(); world.reset(); inventory.reset(); graphics.Release();
        if (StandardAllocator.TotalFreeMemory() != mem1_free || VirtualAllocator.TotalFreeMemory() != mem2_free)
            throw std::runtime_error("Graphics shutdown did not recover both original game arenas");
        log("Original graphics shutdown recovered both game arenas.");
        ResetStartupFiles(); aurora_dvd_close(); session.disc = false;
        ResetStartupMemory(); aurora_shutdown(); session.live = false;
        if (backend_errors || !draws || (!depth_hits && !colour_hits) || (options.frames && frames < options.frames))
            throw std::runtime_error("Static preview incomplete: frames=" + std::to_string(frames) + ", draws=" + std::to_string(draws)
                + ", depth hits=" + std::to_string(depth_hits) + ", colour hits=" + std::to_string(colour_hits) + ", backend errors=" + std::to_string(backend_errors.load()));
        if (options.frontend_boot)
            log("Retail boot screen rendered: " + std::to_string(frames) + " frames, " + std::to_string(draws)
                + " GX draw calls. Original handler prefix executed; full game startup remains pending.");
        else log("Static preview rendered: " + std::to_string(frames) + " frames, " + std::to_string(draws) + " GX draw calls, " + std::to_string(depth_hits) + " geometry depth samples, " + std::to_string(colour_hits) + " visible colour samples. No game scene or gameplay was started.");
        if (world_batch) log("Static world submission totals: " + std::to_string(world_visible)
            + " / " + std::to_string(world_considered) + " objects, " + std::to_string(world_packets) + " packets.");
        if (volume_preview) log("Original stadium shadow blend samples: " + std::to_string(shadow_hits) + ".");
        return 0;
    }
    catch (const std::exception& error) { log(std::string("FAILED: ") + error.what()); return 1; }
}
}
