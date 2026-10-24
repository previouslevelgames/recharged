#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_stdlib.h"

#include "bootstrap/antialiasing.h"
#include "bootstrap/display_shape.h"
#include "bootstrap/launch_options.h"
#include "bootstrap/graphics_backend.h"
#include "launcher/disc_search.h"
#include "launcher/image_library.h"
#include "launcher/issue_reporters.h"
#include "launcher/player_lineup.h"
#include "launcher/release_banner.h"
#include "launcher/release_notes.h"
#include "launcher/ui_kit.h"
#include "launcher/ui_metrics.h"
#include "launcher/update_check.h"
#include "launcher/update_install.h"
#include "launcher/update_service.h"
#include "mscharged/build_version.h"
#include "platform/parse_float.h"
#include "platform/window_fit.h"
#include "platform/disc.h"
#include "platform/game_release.h"
#include "platform/pc_languages.h"
#include "platform/console_verbosity.h"
#include "platform/log_window.h"
#ifdef __APPLE__
#include "platform/macos_app.h"
#endif
#include "platform/app_icon.h"
#include "platform/path.h"
#include "platform/save_import.h"
#include "platform/shader_cache.h"
#include "platform/texture_packs.h"
#include "platform/session_log.h"
#include "platform/gamepad_bindings.h"
#include "platform/graphics_cards.h"
#include "platform/gyro_pointer.h"
#include "platform/key_bindings.h"
#include "platform/launcher_reopen.h"
#include "platform/wiimote_bluetooth.h"
#include "platform/wiimote_hid.h"
#ifdef MSCHARGED_HAS_GAME_STARTUP
#include "runtime/startup.h"
#endif
#include "runtime/scene.h"
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
#include "runtime/original_game.h"
#endif
#ifdef MSCHARGED_WINDOWS_GAME_MODULE_BASE
#include "platform/native_module_loader.h"
#endif
#include <charconv>
#include <cmath>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace mscharged;
using namespace mscharged::launcher;

namespace
{
// Window title of the game and the launcher: the port's name and build.
std::string GameTitle()
{
    std::string title = std::string("Recharged ") + mscharged::build::version;
    if (std::string_view(mscharged::build::channel) == "test")
        title += " - test build of PR #" + std::to_string(mscharged::build::pull_request);
    return title;
}

// The name desktops show for the program (task switcher, audio mixer). SDL
// forgets it at SDL_Quit, so the game sets it again after the launcher.
void SetAppName() { SDL_SetAppMetadata("Recharged", mscharged::build::version, nullptr); }

// The running build: where it comes from (MSCHARGED_BUILD_* in CMakeLists.txt)
// and the package it was made for. The installed package's manifest refines
// the package (the Steam Deck build of Linux) once the update check runs.
BuildIdentity CurrentBuild()
{
    BuildIdentity identity;
    identity.version = build::version;
    const std::string_view channel = build::channel;
    identity.origin = channel == "release" ? BuildOrigin::Release
                    : channel == "test"    ? BuildOrigin::Test
                                           : BuildOrigin::Dev;
    identity.repository = build::repository;
    identity.commit = build::commit;
    identity.pull_request = build::pull_request;
    identity.run = build::run_id;
    identity.built = build::build_time;
    identity.commit_time = build::commit_time;
#if defined(_WIN32)
    identity.package = "windows";
#elif defined(__APPLE__)
    identity.package = "macos";
#else
    identity.package = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    identity.package += "-arm64";
#else
    identity.package += "-x86_64";
#endif
    return identity;
}

// The program, as tools/package_build.py ships it.
#ifdef _WIN32
constexpr const char* kExecutableName = "recharged.exe";
#else
constexpr const char* kExecutableName = "recharged";
#endif

// Seconds since 1970 as local date and time.
std::string LocalTime(std::int64_t seconds)
{
    SDL_DateTime time{};
    if (!SDL_TimeToDateTime(SDL_SECONDS_TO_NS(seconds), &time, true)) return FormatUtcTime(seconds);
    char text[48];
    std::snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d", time.year, time.month, time.day, time.hour,
                  time.minute);
    return text;
}

std::string Megabytes(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return text;
}

// A softly tinted note with an icon and wrapped text, as wide as the content region.
void Callout(const std::string& text, const ImVec4& tint, Icon icon)
{
    const auto& fonts = CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, wrap = width - Dp(58);
    const float size = FontSize(fonts.body);
    const float height = fonts.body->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str()).y + Dp(24);
    const ImVec2 max{pos.x + width, pos.y + height};
    draw->AddRectFilled(pos, max, Col(tint, 0.075f), Dp(12));
    DrawIcon(draw, icon, {pos.x + Dp(23), pos.y + Dp(12) + size * 0.5f}, Dp(16), Col(tint, 0.9f));
    draw->AddText(fonts.body, size, {pos.x + Dp(44), pos.y + Dp(12)}, Col(Mix(color::muted, color::text, 0.7f)),
                  text.c_str(), nullptr, wrap);
    ImGui::Dummy({width, height});
}

enum Page { PagePlay, PageGame, PageDisplay, PageAudio, PageControls, PageGameplay, PageAdvanced, PageAbout, PageCount };
constexpr const char* page_names[] = {"Play", "Game", "Display", "Audio", "Controls", "Gameplay", "Advanced", "About"};
constexpr const char* page_ids[] = {"play", "game", "display", "audio", "controls", "gameplay", "advanced", "about"};
constexpr Icon page_icons[] = {Icon::Play, Icon::Disc, Icon::Display, Icon::Audio, Icon::Keyboard, Icon::Target,
                               Icon::Sliders, Icon::Info};
static_assert(std::size(page_names) == PageCount && std::size(page_ids) == PageCount
    && std::size(page_icons) == PageCount);

#if defined(__linux__)
// The udev rule from docs/SETTINGS.md (Wii Remotes): lets the logged-in user
// open Wii Remotes, paired over Bluetooth or behind a DolphinBar.
constexpr const char* kWiimoteUdevCommands =
    "sudo tee /etc/udev/rules.d/60-recharged-wiimote.rules >/dev/null <<'EOF'\n"
    "KERNEL==\"hidraw*\", SUBSYSTEM==\"hidraw\", KERNELS==\"*057E:0306*\", TAG+=\"uaccess\"\n"
    "KERNEL==\"hidraw*\", SUBSYSTEM==\"hidraw\", KERNELS==\"*057E:0330*\", TAG+=\"uaccess\"\n"
    "EOF\n"
    "sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw\n";
#endif

struct Options
{
    fs::path config;
    LaunchOptions launch;
    fs::path screenshot;
    bool launcher = false;
    bool smoke_test = false;
    bool experimental_startup = false;
    bool experimental_scene = false;
    bool experimental_frontend = false;
    bool experimental_options = false;
    bool frame_arguments = false;
    bool scene_arguments = false;
    bool standalone_assets = false;
    bool page_selected = false;
    SceneOptions scene;
    int page = PagePlay;
};

void Require(bool ok, const char* context)
{
    if (!ok) throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

fs::path DefaultConfig(const fs::path& executable_directory)
{
    return DefaultConfigPath(executable_directory);
}

struct DialogResult
{
    std::mutex mutex;
    bool active = false;
    bool ready = false;
    std::string path;
    std::string error;
};

void SDLCALL DiscChosen(void* data, const char* const* files, int)
{
    // The callback can outlive the window or run on a different thread.
    std::unique_ptr<std::shared_ptr<DialogResult>> owner(static_cast<std::shared_ptr<DialogResult>*>(data));
    auto& result = **owner;
    std::lock_guard<std::mutex> lock(result.mutex);
    result.path = files && files[0] ? files[0] : "";
    result.error = files ? "" : SDL_GetError();
    result.ready = true;
    result.active = false;
}

// A check of one image: the pinned one, or one chosen with Browse, a drop or
// Locate, before it is pinned.
struct DiscCheck
{
    std::optional<DiscInfo> info;
    std::string error;
    std::string selection;
    std::string missing; // why the file cannot be found (MissingImageReason); empty when it exists
    bool folder_missing = false; // its folder (or drive) is gone too
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    std::vector<std::uint8_t> banner; // the release's banner (RGBA), when it was not known yet
    bool banner_read = false;         // a banner read was tried
};

DiscCheck RunDiscCheck(const fs::path& path, std::string selection, const std::set<std::string>& banners_known)
{
    DiscCheck result;
    result.selection = std::move(selection);
    result.missing = MissingImageReason(path);
    if (!result.missing.empty())
    {
        std::error_code error;
        result.folder_missing = !fs::is_directory(path.parent_path(), error);
        result.error = result.missing;
        return result;
    }
    std::error_code error;
    result.size = launcher::DiscImageSize(path, error);
    const auto time = fs::last_write_time(path, error);
    if (!error) result.modified = std::int64_t(time.time_since_epoch().count());
    try { result.info = InspectDisc(path); }
    catch (const std::exception& e)
    {
        result.error = e.what();
        return result;
    }
    if (!banners_known.count(result.info->game_id))
    {
        result.banner_read = true;
        try { result.banner = ReadReleaseBanner(path, result.info->game_id); }
        catch (const std::exception&) {}
    }
    return result;
}

// "USA, revision 1": the region and revision, always both (the launcher's
// own labels leave out a revision where a region has only one).
std::string RegionRevision(const DiscInfo& info)
{
    return std::string(DiscRegionName(info.game_id)) + ", revision " + std::to_string(unsigned(info.revision));
}

// The release's title, or the disc header's for a release Recharged does not know.
std::string ReleaseTitle(const DiscInfo& info)
{
    LibraryImage image;
    image.probe.kind = DiscProbe::Kind::Charged;
    image.probe.info = info;
    return ImageTitle(image);
}

char RegionLetter(const std::string& game_id) { return game_id.size() == 6 ? game_id[3] : '?'; }

// A disc ID usable as a file name.
bool SafeGameId(const std::string& id)
{
    return id.size() == 6 && std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
}

enum class NoticeKind { Info, Success, Warning, Error };
enum class FolderPurpose { SaveFolder, DolphinSave, GameFolder, TextureFolder };

// The save a release keeps: its title and the identity the runtime gives its
// storage (InitializeNativeFilesystemForDisc in original_game.cpp).
mscharged::platform::SaveTitle SaveTitleOf(const GameRelease& release)
{
    return {release.title_id, 0x1001, release.title_group};
}
constexpr const char* kSaveFile = "Strikers2";

std::string FileUrl(const fs::path& path)
{
    std::string generic = PathUtf8(fs::absolute(path));
#ifdef _WIN32
    std::replace(generic.begin(), generic.end(), '\\', '/');
    std::string url = "file:///";
#else
    std::string url = "file://";
#endif
    for (const unsigned char c : generic)
    {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') url += char(c);
        else
        {
            char escaped[4];
            std::snprintf(escaped, sizeof escaped, "%%%02X", unsigned(c));
            url += escaped;
        }
    }
    return url;
}

std::string Quote(const std::string& text)
{
    std::string quoted = "\"";
    for (const char c : text) { if (c == '"' || c == '\\') quoted += '\\'; quoted += c; }
    return quoted + "\"";
}

class Launcher
{
public:
    const std::optional<ResolvedLaunch>& FrontendLaunch() const { return frontend_launch_; }
    // An update was installed: main() starts the updated launcher.
    bool RestartRequested() const { return restart_; }
    fs::path RestartExecutable() const { return InstalledLauncher(base_); }

    ResolvedLaunch EffectiveLaunch() const { return ResolveLaunch(file_, options_.launch, &draft_); }
    std::string DiscSelection() const { return PathUtf8(EffectiveLaunch().disc_path); }

    ~Launcher()
    {
        wii_probe_.Close();
        remote_lights_.reset(); // a remote that still rumbles stops first
        if (renderer_ui_ready_) ImGui_ImplSDLRenderer3_Shutdown();
        if (window_ui_ready_) ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        for (auto* pad : pads_) SDL_CloseGamepad(pad);
        // A running library scan stops at its next image; its future waits for it.
        if (library_progress_) library_progress_->cancel = true;
        for (auto& [id, texture] : banners_)
            if (texture) SDL_DestroyTexture(texture);
        if (header_) SDL_DestroyTexture(header_);
        if (aurora_logo_) SDL_DestroyTexture(aurora_logo_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    int Run(Options options)
    {
        options_ = std::move(options);
        Require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "Cannot initialize SDL");
        const char* base_path = SDL_GetBasePath();
        Require(base_path != nullptr, "Cannot locate launcher resources");
        base_ = PathFromUtf8(base_path);
        if (options_.config.empty()) options_.config = DefaultConfig(base_);
        file_.path = fs::absolute(options_.config);
        Reload();
        // The image library's cache and the banners read from the player's images.
        library_data_ = DataFolder(base_) / "launcher";
        image_cache_ = LoadImageCache(library_data_ / "library.json");
        page_ = options_.page;
        capture_ = options_.smoke_test || !options_.screenshot.empty();
        StartUpdates();
#if !defined(__APPLE__)
        // The PC's graphics cards, for Advanced -> Graphics card and the log.
        graphics_cards_scan_ = std::async(std::launch::async, platform::ListGraphicsCards);
        graphics_cards_listing_ = true;
#endif

        // Created at the design size, then fitted once the window knows its
        // display's pixel density and content scale.
        window_ = SDL_CreateWindow(GameTitle().c_str(), int(kDefaultDesignWidth),
                                   int(kDefaultDesignHeight),
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        Require(window_ != nullptr, "Cannot create launcher window");
        platform::SetApplicationIcon(window_);
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        Require(renderer_ != nullptr, "Cannot create launcher renderer");
        SDL_SetRenderVSync(renderer_, 1);
        metrics_ = CurrentMetrics();
        ApplyWindowMetrics(true);

        const auto resources = base_ / "assets/launcher";
        auto surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
            SDL_LoadPNG(PathUtf8(resources / "header.png").c_str()), SDL_DestroySurface);
        Require(surface != nullptr, "Cannot load launcher header; rebuild to restore resources");
        header_width_ = float(surface->w);
        header_height_ = float(surface->h);
        header_ = SDL_CreateTextureFromSurface(renderer_, surface.get());
        Require(header_ != nullptr, "Cannot create header texture");
        SDL_SetTextureScaleMode(header_, SDL_SCALEMODE_LINEAR);
        // Aurora's logo (from its pinned source) is optional decoration.
        if (auto logo = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
                SDL_LoadPNG(PathUtf8(resources / "aurora.png").c_str()), SDL_DestroySurface))
        {
            aurora_logo_width_ = float(logo->w);
            aurora_logo_height_ = float(logo->h);
            aurora_logo_ = SDL_CreateTextureFromSurface(renderer_, logo.get());
            if (aurora_logo_) SDL_SetTextureScaleMode(aurora_logo_, SDL_SCALEMODE_LINEAR);
        }
        font_path_ = PathUtf8(resources / "Roboto-Medium.ttf");
        if (!fs::exists(resources / "Roboto-Medium.ttf"))
            throw std::runtime_error("Missing launcher font; rebuild to restore resources");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr; // UI state must not create a public imgui.ini.
        io.LogFilename = nullptr;
        RebuildScale();
        Require(ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_), "Cannot initialize window UI");
        window_ui_ready_ = true;
        Require(ImGui_ImplSDLRenderer3_Init(renderer_), "Cannot initialize UI renderer");
        renderer_ui_ready_ = true;
        RefreshGamepad();
        RefreshDisplays();
        // Screenshots on a real video driver map the window so it learns its
        // display scale; headless drivers render the hidden window directly.
        const char* video = SDL_GetCurrentVideoDriver();
        const bool headless = video && (std::string(video) == "dummy" || std::string(video) == "offscreen");
        if (!capture_ || (!options_.screenshot.empty() && !headless)) SDL_ShowWindow(window_);
#ifdef _WIN32
        // Reopened by a game that closed (launcher.reopen_after_game) or
        // restarted after an update: come to the front, which the previous
        // process allowed, rather than behind other windows.
        if (!capture_) SDL_RaiseWindow(window_);
#endif
        if (config_ok_ && !EffectiveLaunch().disc_path.empty() && !options_.smoke_test) CheckDisc();
        StartDiscSearch();

        bool quit = false;
        unsigned frame = 0;
        const unsigned smoke_frames = unsigned(PageCount) * 4 + 2;
        while (!quit)
        {
            const Uint64 frame_start = SDL_GetTicks();
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                const bool capturing = capture_action_ >= 0 || hotkey_capture_ >= 0;
                if (!capturing || (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_TEXT_INPUT))
                    ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                    && event.window.windowID == SDL_GetWindowID(window_)))
                    close_requested_ = true;
                if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
                    RefreshGamepad();
                if (event.type == SDL_EVENT_DISPLAY_ADDED || event.type == SDL_EVENT_DISPLAY_REMOVED)
                    RefreshDisplays();
                // On the Gameplay page a dropped texture pack is added; anything
                // else is a game image or folder, as on the other pages.
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data
                    && !(page_ == PageGameplay && config_ok_ && AddTexturePack(PathFromUtf8(event.drop.data))))
                    SelectDisc(event.drop.data);
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && capture_action_ >= 0)
                    CaptureKey(event.key.scancode);
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && hotkey_capture_ >= 0)
                    CaptureHotkey(event.key.scancode, event.key.mod);
                // Middle and side mouse buttons can take a key's place (#62).
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && capture_action_ >= 0
                    && platform::MouseKeyInput(event.button.button) != platform::kKeyNone)
                    CaptureKey(platform::MouseKeyInput(event.button.button));
                GamepadCaptureEvent(event);
                GyroPreviewEvent(event);
            }
            // The gyro preview takes readings only while it is on screen.
            if (!gyro_preview_drawn_) StopGyroPreview();
            gyro_preview_drawn_ = false;
            // While an input waits for a gamepad press, and until the press
            // that set it is released, the gamepad does not drive the launcher.
            auto& io = ImGui::GetIO();
            if (pad_capture_release_ && pad_inputs_held_.empty()) pad_capture_release_ = false;
            if (pad_capture_action_ >= 0 || pad_capture_release_) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
            else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            PollResults();
            SyncLibrary();
            // Moving between displays or changing the interface size rebuilds
            // fonts and style before the frame starts.
            const auto metrics = CurrentMetrics();
            if (std::fabs(metrics.ui - metrics_.ui) > 0.005f || std::fabs(metrics.density - metrics_.density) > 0.005f)
            {
                metrics_ = metrics;
                RebuildScale();
                ApplyWindowMetrics(false);
            }
            SetUiScale(metrics_.ui);
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (options_.smoke_test) page_ = int(frame / 4) % PageCount;
            hero_visible_ = false;
            Draw(quit);
            ImGui::Render();
            // ImGui works in window units; the renderer targets framebuffer
            // pixels. Without this scale a retina display shows the UI in one
            // corner while input arrives in window units.
            SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer_, 10, 14, 23, 255);
            SDL_RenderClear(renderer_);
            RenderHero();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
            const bool update_settled = update_preview_ || (!UpdateBusy() && !(update_capture_install_ && update_.offer));
            if (!options_.screenshot.empty() && frame >= 12
                && ((!pending_ && !searching_ && !library_scanning_ && !candidate_pending_ && !texture_scanning_
                     && update_settled)
                    || frame > 600))
            {
                SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
                auto pixels = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
                    SDL_RenderReadPixels(renderer_, nullptr), SDL_DestroySurface);
                Require(pixels != nullptr, "Cannot capture launcher");
                Require(SDL_SavePNG(pixels.get(), PathUtf8(options_.screenshot).c_str()), "Cannot save screenshot");
                quit = true;
            }
            SDL_RenderPresent(renderer_);
            ++frame;
            if (options_.smoke_test && frame >= smoke_frames) quit = true;
            const auto elapsed = SDL_GetTicks() - frame_start;
            if (elapsed < 16) SDL_Delay(Uint32(16 - elapsed));
        }
        if (options_.smoke_test)
            std::cout << "Launcher rendered all " << int(PageCount) << " pages and loaded its resources.\n";
        return 0;
    }

private:
    // ---------------------------------------------------------------- scale
    UiMetrics CurrentMetrics() const
    {
        const DisplayScale scale{SDL_GetWindowDisplayScale(window_), SDL_GetWindowPixelDensity(window_)};
        float user = ParseUiScaleSetting(draft_.ui_scale);
        if (user < 0.0f) user = 0.0f;
        SDL_Rect usable{};
        float width = 0.0f, height = 0.0f;
        if (!capture_ && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window_), &usable))
        { width = float(usable.w); height = float(usable.h); }
        return ComputeUiMetrics(scale, user, width, height);
    }

    void ApplyWindowMetrics(bool initial)
    {
        SDL_Rect usable{};
        float width = 0.0f, height = 0.0f;
        if (!capture_ && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window_), &usable))
        { width = float(usable.w); height = float(usable.h); }
        const auto minimum = FitWindow(kMinimumDesignWidth, kMinimumDesignHeight, metrics_.ui, width, height);
        SDL_SetWindowMinimumSize(window_, minimum.width, minimum.height);
        if (initial)
        {
            const auto size = FitWindow(kDefaultDesignWidth, kDefaultDesignHeight, metrics_.ui, width, height);
            SDL_SetWindowSize(window_, size.width, size.height);
            SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }

    void RebuildScale()
    {
        SetUiScale(metrics_.ui);
        LoadFonts(font_path_, metrics_.font_raster, metrics_.density);
        ApplyTheme(metrics_.ui);
        if (renderer_ui_ready_) ImGui_ImplSDLRenderer3_DestroyFontsTexture(); // recreated by NewFrame
    }

    void RenderHero()
    {
        // The artwork is drawn through SDL (also avoiding large textured
        // triangles in the software renderer), cropped like CSS "cover";
        // ImGui then paints its gradients and text over it.
        if (!hero_visible_ || hero_.w <= 0 || hero_.h <= 0) return;
        SDL_FRect source{0, 0, header_width_, header_height_};
        const float target_ratio = hero_.w / hero_.h;
        if (header_width_ / header_height_ > target_ratio)
        {
            source.w = header_height_ * target_ratio;
            source.x = (header_width_ - source.w) * 0.5f;
        }
        else
        {
            source.h = header_width_ / target_ratio;
            source.y = (header_height_ - source.h) * 0.40f;
        }
        SDL_RenderTexture(renderer_, header_, &source, &hero_);
    }

    // ------------------------------------------------------------ settings
    void Notice(std::string text, NoticeKind kind)
    {
        notice_ = std::move(text);
        notice_kind_ = kind;
    }

    void Reload()
    {
        try
        {
            file_ = LoadConfig(file_.path, true);
            draft_ = file_.settings;
            mscharged::platform::SetVerboseConsole(file_.settings.verbose_console);
            mscharged::platform::SetLogWindow(file_.settings.log_window);
            dirty_ = false;
            config_ok_ = true;
            // The pinned image's check stays while the image is the same (a
            // reverted library choice brings the old one back: check it).
            if (check_.selection != DiscSelection())
            {
                check_ = {};
                if (renderer_ && !pending_) CheckDisc();
            }
            Notice(file_.exists ? "Settings loaded." : "Welcome! Add your game folder or choose your game image to get started.",
                   NoticeKind::Info);
        }
        catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); config_ok_ = false; }
    }

    bool Save()
    {
        try
        {
            SaveConfig(file_, draft_);
            mscharged::platform::SetVerboseConsole(draft_.verbose_console);
            mscharged::platform::SetLogWindow(draft_.log_window);
            dirty_ = false;
            Notice("Settings saved.", NoticeKind::Success);
            return true;
        }
        catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); return false; }
    }

    // ------------------------------------------------------------- updates
    void StartUpdates()
    {
        updates_ = std::make_unique<UpdateService>(CurrentBuild(), base_);
        // Development captures stay offline unless RECHARGED_CAPTURE_UPDATES
        // asks for the real check: "dialog" also opens an offered update,
        // "install" installs it and restarts (the screenshot comes from the
        // restarted launcher), "preview-..." shows a made-up state.
        const char* capture_updates = std::getenv("RECHARGED_CAPTURE_UPDATES");
        if (!capture_updates) capture_updates = std::getenv("MSCHARGED_CAPTURE_UPDATES"); // before the rename (#64)
        if (capture_ && !(capture_updates && *capture_updates)) return;
        if (capture_)
        {
            // "preview-<state>[-dialog|-menu]" shows a made-up state offline
            // (UpdateService::Preview), optionally with the update dialog or
            // the Test menu open.
            std::string mode = capture_updates;
            if (mode.rfind("preview-", 0) == 0)
            {
                mode.erase(0, 8);
                auto suffix = [&mode](const std::string& text) {
                    const bool found = mode.size() > text.size()
                        && mode.compare(mode.size() - text.size(), text.size(), text) == 0;
                    if (found) mode.resize(mode.size() - text.size());
                    return found;
                };
                const bool dialog = suffix("-dialog"), menu = !dialog && suffix("-menu");
                update_preview_ = updates_->Preview(mode);
                update_dialog_ = update_preview_ && dialog;
                test_menu_ = update_preview_ && menu;
                // The switch shows the previewed channel and choice (not saved).
                if (update_preview_)
                {
                    const auto preview = updates_->Snapshot();
                    draft_.update_channel = preview.channel == UpdateChannel::Test ? "test" : "release";
                    draft_.test_build = preview.chosen;
                }
                return;
            }
            update_capture_dialog_ = mode == "dialog";
            update_capture_install_ = mode == "install";
        }
        if (!capture_)
        {
            try
            {
                // What the last update left behind; say once that it worked.
                const auto replaced = FinishUpdates(base_);
                if (const auto text = replaced.empty() ? std::string() : DescribeReplacement(replaced, CurrentBuild());
                    !text.empty())
                    Notice(text, NoticeKind::Success);
            }
            catch (const std::exception& error) { Notice(error.what(), NoticeKind::Warning); }
        }
        if (config_ok_ && (draft_.check_updates || capture_)) CheckForUpdates(UpdatePrompt::Newer, UpdateChannelSetting());
    }

    // The update dialog opens by itself when a check finds a build: after the
    // start only for a newer one; after the Release / Test switch or a click on
    // the status also for the way back to the release. After the switch to
    // Test with several test builds and none followed, the Test menu opens.
    // Reveal: a Ctrl+click on Test, which then opens the menu with the hidden test builds.
    enum class UpdatePrompt { None, Newer, Any, Switch, Reveal };

    void CheckForUpdates(UpdatePrompt prompt, UpdateChannel channel)
    {
        update_prompt_ = capture_ ? UpdatePrompt::None : prompt;
        updates_->Check(channel, draft_.test_build, show_hidden_tests_);
    }

    bool UpdateBusy() const
    {
        using State = UpdateService::State;
        return update_.state == State::Checking || update_.state == State::Downloading
            || update_.state == State::Installing || update_.state == State::Installed;
    }

    UpdateChannel UpdateChannelSetting() const
    {
        return ResolveChannel(draft_.update_channel, updates_ ? updates_->Build() : CurrentBuild());
    }

    // The Play page's Release / Test switch and the Test menu's choice are
    // saved at once, without other unsaved changes, so they never hold an
    // update back.
    void SaveUpdateChoice(const std::string& channel, int test_build)
    {
        draft_.update_channel = channel;
        draft_.test_build = test_build;
        if (!config_ok_) return;
        try
        {
            Settings saved = file_.settings;
            saved.update_channel = channel;
            saved.test_build = test_build;
            SaveConfig(file_, saved);
        }
        catch (const std::exception& error) { Notice(error.what(), NoticeKind::Error); }
    }

    // Release leaves the test builds; Test asks which one when several are open.
    void SelectUpdateChannel(UpdateChannel channel)
    {
        BeginSwitch();
        const bool test = channel == UpdateChannel::Test;
        SaveUpdateChoice(test ? "test" : "release", test ? draft_.test_build : 0);
        CheckForUpdates(test ? UpdatePrompt::Switch : UpdatePrompt::Any, channel);
    }

    // A test build chosen in the Test menu: followed from now on, offered
    // with its notes when it is not the running build.
    void SelectTestBuild(int pull_request)
    {
        BeginSwitch();
        SaveUpdateChoice("test", pull_request);
        CheckForUpdates(UpdatePrompt::Any, UpdateChannel::Test);
    }

    // A switch (Release / Test, or a test build from the menu) holds only
    // when something gets installed: Later in the update dialog, or closing
    // the menu without a choice, brings the previous choice back, since
    // nothing changed. Without anything to install (no test builds, no
    // network) the new choice stays.
    void BeginSwitch()
    {
        if (!switch_from_) switch_from_ = UpdateChoice{draft_.update_channel, draft_.test_build};
    }

    void CancelSwitch()
    {
        if (!switch_from_) return;
        const UpdateChoice previous = *switch_from_;
        switch_from_.reset();
        SaveUpdateChoice(previous.channel, previous.test_build);
        CheckForUpdates(UpdatePrompt::None, UpdateChannelSetting());
    }

    // Later (or Escape / B) in the update dialog.
    void DismissUpdateDialog()
    {
        ImGui::CloseCurrentPopup();
        CancelSwitch();
    }

    // On the test channel, test builds are published but none is followed
    // (several pull requests are open, or the one followed has closed).
    bool ChooseTestBuild() const
    {
        return update_.channel == UpdateChannel::Test && update_.state == UpdateService::State::UpToDate
            && !update_.track && !update_.test_builds.empty();
    }

    // Test gets its menu when there is a choice to make.
    bool TestMenuAvailable() const
    {
        using State = UpdateService::State;
        if (update_.channel != UpdateChannel::Test || update_.state == State::Downloading
            || update_.state == State::Installing || update_.state == State::Installed)
            return false;
        return update_.test_builds.size() >= 2 || ChooseTestBuild();
    }

    // The update status: a glass pill over the artwork, tinted with the
    // offer's colour when a check found a build. A click checks again or
    // opens what was found.
    enum class PillAction { None, Check, Open };
    struct PillLook
    {
        Icon icon = Icon::None;
        bool spinner = false;
        std::string text, tooltip;
        ImVec4 tint = color::muted;
        bool filled = false; // tinted and glowing: a check found a build
        PillAction action = PillAction::None;
        float progress = -1.0f; // a download's progress, 0..1
    };

    // Green for updates, amber for test builds, blue for the way back to the release.
    static ImVec4 OfferTint(OfferKind kind, const PublishedBuild& offer)
    {
        if (kind == OfferKind::BackToRelease) return color::info;
        return offer.test ? color::warning : color::accent;
    }

    static Icon OfferIcon(OfferKind kind, const PublishedBuild& offer)
    {
        // What gets installed: a release (its tag, also for the way back), a test build or an update.
        if (kind == OfferKind::BackToRelease) return Icon::Tag;
        return offer.test ? Icon::Flask : Icon::Download;
    }

    std::string LastChecked() const
    {
        return update_.checked ? "Checked at " + LocalTime(update_.checked).substr(11) + "." : std::string();
    }

    PillLook UpdatePillLook() const
    {
        using State = UpdateService::State;
        const bool test = update_.channel == UpdateChannel::Test;
        const std::string again = "\nClick to check again.";
        const std::string checked = update_.checked ? "\n" + LastChecked() : std::string();
        PillLook look;
        switch (update_.state)
        {
        case State::Idle:
            look = {Icon::Refresh, false, "Check for updates",
                    draft_.check_updates ? "Looks for a newer build on GitHub."
                                         : "Automatic checks are off (Advanced page).\nClick to check now.",
                    color::muted};
            break;
        case State::Checking: look = {Icon::None, true, "Checking for updates...", "", color::info}; break;
        case State::UpToDate:
            if (ChooseTestBuild())
            {
                // Test builds to choose from: the click opens the Test menu.
                const auto count = update_.test_builds.size();
                look = {Icon::Flask, false, count == 1 ? "Test build available" : "Choose a test build",
                        (count == 1 ? std::string("A test build is published.")
                                    : std::to_string(count) + " test builds are published, one for each change "
                                                              "being tested.")
                            + "\nClick to choose which one to follow.",
                        color::warning};
                look.filled = true;
                look.action = PillAction::Open;
                return look;
            }
            // Switched to Test while no test build is published: say why nothing changes.
            if (test && update_.test_builds.empty() && updates_->Build().origin != BuildOrigin::Test)
                look = {Icon::Check, false, "No test builds",
                        "No test build is published right now. Test builds are published while pull requests "
                        "are open."
                            + checked + again,
                        color::warning};
            else
                look = {Icon::Check, false, "Up to date",
                        std::string("No newer ") + (test ? "release or test build" : "release") + " than this build."
                            + checked + again,
                        color::accent};
            look.action = PillAction::Check;
            break;
        case State::CheckFailed:
            // Often just offline: shown quietly, the reason in the tooltip.
            look = {Icon::Warning, false, "Couldn't check for updates", update_.error + again, color::muted};
            look.action = PillAction::Check;
            break;
        case State::Available:
        {
            const OfferKind kind = ClassifyOffer(updates_->Build(), *update_.offer);
            look.icon = OfferIcon(kind, *update_.offer);
            look.tint = OfferTint(kind, *update_.offer);
            look.filled = true;
            look.action = PillAction::Open;
            switch (kind)
            {
            case OfferKind::Update:
                look.text = update_.offer->test ? "New test build"
                                                : "Version " + VersionNumber(update_.offer->version) + " available";
                look.tooltip = "See what's new and update.";
                break;
            case OfferKind::TestBuild:
                look.text = "Test build available";
                look.tooltip = "See what it changes and switch to it.";
                break;
            case OfferKind::BackToRelease:
                look.text = "Back to release " + VersionNumber(update_.offer->version);
                look.tooltip = "Replace this test build by the official release.";
                break;
            }
            break;
        }
        case State::Downloading:
        {
            const auto percent = update_.total ? update_.received * 100 / update_.total : 0;
            look = {Icon::Download, false, "Downloading " + std::to_string(percent) + "%", "Click to see the progress.",
                    color::accent};
            look.progress = update_.total ? float(double(update_.received) / double(update_.total)) : 0.0f;
            break;
        }
        case State::Installing: look = {Icon::None, true, "Installing...", "", color::info}; break;
        case State::Installed: look = {Icon::None, true, "Restarting...", "", color::accent}; break;
        case State::InstallFailed:
            look = {Icon::Error, false, "Update failed", update_.error, color::danger};
            look.action = PillAction::Open;
            break;
        }
        return look;
    }

    static float PillWidth(const PillLook& look)
    {
        return Dp(38) + TextSize(CurrentFonts().body, look.text.c_str()).x
            + (look.action == PillAction::None ? Dp(18) : Dp(42));
    }

    bool DrawUpdatePill(const PillLook& look, ImVec2 size)
    {
        const auto& fonts = CurrentFonts();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##update_status", size);
        const bool held = ImGui::IsItemActive();
        const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 12.0f);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 max{pos.x + size.x, pos.y + size.y};
        const float rounding = size.y * 0.5f, middle = pos.y + size.y * 0.5f;
        const ImVec4 white{1, 1, 1, 1};
        ImU32 ink, label;
        if (look.filled)
        {
            // Something was found: filled with its colour, glowing softly.
            const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.2f);
            for (int i = 0; i < 4; ++i)
            {
                const float grow = Dp(1.5f + 2.5f * float(i));
                draw->AddRectFilled({pos.x - grow, pos.y - grow}, {max.x + grow, max.y + grow},
                                    Col(look.tint, (0.075f - 0.015f * float(i)) * (0.6f + 0.4f * pulse + 0.4f * hover)),
                                    rounding + grow);
            }
            // Tinted glass: the colour in the outline, icon and a soft wash.
            const float strength = (held ? 0.20f : 0.26f) + 0.10f * hover;
            FillGradient(draw, pos, max, Col(Mix(color::window, look.tint, strength + 0.08f)),
                         Col(Mix(color::window, look.tint, strength * 0.55f)), rounding, true);
            draw->AddRect(pos, max, Col(look.tint, 0.80f + 0.20f * hover), rounding, 0, Dp(1));
            ink = Col(Mix(look.tint, white, 0.15f));
            label = Col(color::text);
        }
        else
        {
            // Glass over the artwork; a download fills it from the left.
            draw->AddRectFilled(pos, max, Col(Mix(color::window, color::card_hover, 0.45f + 0.55f * hover), 0.9f),
                                rounding);
            if (look.progress >= 0.0f)
                FillGradient(draw, pos, {pos.x + std::max(size.y, size.x * std::min(look.progress, 1.0f)), max.y},
                             Col(Mix(color::window, look.tint, 0.20f)), Col(Mix(color::window, look.tint, 0.36f)),
                             rounding, true);
            draw->AddRect(pos, max, Col(Mix(color::border, look.tint, 0.25f + 0.35f * hover), 0.95f), rounding, 0,
                          Dp(1));
            ink = Col(look.tint);
            label = Col(Mix(color::muted, color::text, 0.65f + 0.35f * hover));
        }
        const ImVec2 icon{pos.x + Dp(20), middle};
        if (look.spinner) Spinner(icon, Dp(7), ink);
        else DrawIcon(draw, look.icon, icon, Dp(16), ink);
        const ImVec2 text = TextSize(fonts.body, look.text.c_str());
        DrawLabel(draw, fonts.body, {pos.x + Dp(38), middle - text.y * 0.5f}, label, look.text.c_str());
        // What a click does: check again, or open what was found.
        const ImVec2 end{max.x - Dp(21), middle};
        if (look.action == PillAction::Check)
            DrawIcon(draw, Icon::Refresh, end, Dp(15), Col(Mix(color::muted, color::text, 0.15f + 0.85f * hover)));
        else if (look.action == PillAction::Open)
            DrawChevron(draw, {end.x + Dp(2) * hover, end.y}, Dp(13), look.filled ? ink : Col(look.tint));
        return pressed;
    }

    // Top right of the Play page's artwork: the update status and the
    // Release / Test switch.
    void DrawUpdateBar(ImVec2 origin, float width, float pad)
    {
        if (!updates_) return;
        using State = UpdateService::State;
        const float height = Dp(38), switch_width = Dp(206);
        const PillLook look = UpdatePillLook();
        const float y = origin.y + Dp(18);
        float x = origin.x + width - pad - switch_width;
        ImGui::SetCursorScreenPos({x, y});
        int channel = UpdateChannelSetting() == UpdateChannel::Test ? 1 : 0;
        const bool busy = update_.state == State::Downloading || update_.state == State::Installing
            || update_.state == State::Installed;
        const bool menu = TestMenuAvailable();
        int pressed = -1;
        ImGui::BeginDisabled(busy || !config_ok_);
        const bool switched = SegmentedIcons(
            "##update_channel", &channel,
            {{"Release", Icon::Tag, color::accent}, {"Test", Icon::Flask, color::warning, menu}},
            {switch_width, height}, true, &pressed);
        // Ctrl+click (Cmd+click on a Mac) on Test, a hidden switch: the test
        // builds their pull request hides (ShownBuilds) show until the
        // launcher closes, or hide again.
        const bool reveal = pressed == 1 && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper);
        if (reveal)
        {
            show_hidden_tests_ = !show_hidden_tests_;
            Notice(show_hidden_tests_ ? "Hidden test builds are shown until the launcher closes."
                                      : "Hidden test builds are hidden again.",
                   NoticeKind::Info);
        }
        if (switched) SelectUpdateChannel(channel ? UpdateChannel::Test : UpdateChannel::Release);
        else if (reveal) CheckForUpdates(UpdatePrompt::Reveal, UpdateChannel::Test);
        // Test again, with a choice: the menu, unless this mouse click closed it.
        else if (pressed == 1 && menu
                 && !(ImGui::GetIO().MouseReleased[0] && test_menu_closed_ >= ImGui::GetIO().MouseClickedTime[0]))
            test_menu_ = true;
        ImGui::EndDisabled();
        const ImVec2 menu_anchor{ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + Dp(8)};
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(menu ? "Release: official releases.\n"
                                     "Test: test builds of changes before they are released; click Test again to "
                                     "choose which one.\n"
                                     "Nothing is installed without asking."
                                   : "Release: official releases.\n"
                                     "Test: also the test builds of pull requests, to try changes before they are "
                                     "released.\n"
                                     "Nothing is installed without asking.");
        const float pill_width = PillWidth(look);
        x -= Dp(10) + pill_width;
        ImGui::SetCursorScreenPos({x, y});
        if (DrawUpdatePill(look, {pill_width, height}))
        {
            if (ChooseTestBuild()) test_menu_ = true;
            else if (update_.state == State::Idle || update_.state == State::UpToDate
                     || update_.state == State::CheckFailed)
                CheckForUpdates(UpdatePrompt::Any, UpdateChannelSetting());
            else if (update_.offer)
                update_dialog_ = true;
        }
        if (!look.tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", look.tooltip.c_str());
        DrawTestBuildMenu(menu_anchor);
    }

    // What a test build's title says besides its version: CI names them
    // "Test build: <pull request title> (#N)", release pull requests are
    // titled "Release 1.1.0 – NetCode!".
    static std::string TestBuildSubtitle(const PublishedBuild& build)
    {
        std::string rest;
        TitleVersion(build, &rest);
        return ReleaseSubtitle(rest, build.version);
    }

    // The version a test build's title starts with ("Release 1.0.x – Nintendo
    // Switch build" gives "1.0.x"), and the title's rest; empty when it names
    // none. A pull request that never raised its version builds packages with
    // the last release's number, which would pass it off as that release.
    static std::string TitleVersion(const PublishedBuild& build, std::string* rest = nullptr)
    {
        std::string title = build.title;
        const std::string prefix = "Test build: ", suffix = " (#" + std::to_string(build.pull_request) + ")";
        if (title.rfind(prefix, 0) == 0) title.erase(0, prefix.size());
        if (title.size() > suffix.size() && title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0)
            title.resize(title.size() - suffix.size());
        if (title.rfind("Release ", 0) == 0) title.erase(0, 8);
        if (rest) *rest = title;
        // Digits, then dotted parts of digits or x, then the title's end or a separator.
        auto part = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == 'x' || c == 'X'; };
        std::size_t end = 0;
        while (end < title.size() && std::isdigit(static_cast<unsigned char>(title[end]))) ++end;
        bool dotted = false;
        while (end > 0 && end + 1 < title.size() && title[end] == '.' && part(title[end + 1]))
        {
            for (++end; end < title.size() && part(title[end]); ++end) {}
            dotted = true;
        }
        if (!dotted || (end < title.size() && title[end] != ' ' && title[end] != ':' && title[end] != '-')) return {};
        if (rest) *rest = title.substr(end);
        return title.substr(0, end);
    }

    // What a test build shows as its version: its title's, else its packages'.
    static std::string TestBuildNumber(const PublishedBuild& build)
    {
        const std::string titled = TitleVersion(build);
        return titled.empty() ? VersionNumber(build.version) : titled;
    }

    // `text` cut to `width` with an ellipsis, at a UTF-8 character boundary.
    static std::string Elide(ImFont* font, std::string text, float width)
    {
        if (TextSize(font, text.c_str()).x <= width) return text;
        const std::string ellipsis = "\xE2\x80\xA6";
        while (!text.empty() && TextSize(font, (text + ellipsis).c_str()).x > width)
        {
            do text.pop_back();
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);
            while (!text.empty() && text.back() == ' ') text.pop_back();
        }
        return text + ellipsis;
    }

    // The Test menu below the switch: the newest test build of each open
    // pull request. A click follows that one and offers it in the update
    // dialog; the running build's own is marked as installed.
    void DrawTestBuildMenu(ImVec2 anchor)
    {
        if (test_menu_)
        {
            test_menu_ = false;
            if (!update_.test_builds.empty()) ImGui::OpenPopup("##test_builds");
        }
        const auto& fonts = CurrentFonts();
        const float width = Dp(392);
        ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, {1.0f, 0.0f});
        ImGui::SetNextWindowSize({width, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(10, 14));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0, 0, 0, 0});
        const bool open = ImGui::BeginPopup("##test_builds", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!open)
        {
            if (test_menu_open_)
            {
                test_menu_closed_ = ImGui::GetTime();
                // Closed without a choice: a switch to Test that waited for one is undone.
                if (!test_menu_chose_) CancelSwitch();
            }
            test_menu_open_ = test_menu_chose_ = false;
            return;
        }
        test_menu_open_ = true;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Dp(10), 0.0f});
        DrawDialogCard(color::warning, Dp(14), Dp(6));
        auto* draw = ImGui::GetWindowDrawList();
        const float left = ImGui::GetCursorScreenPos().x;
        DrawTracked(draw, fonts.caption, {left + Dp(8), ImGui::GetCursorScreenPos().y}, Col(color::dim), "TEST BUILDS",
                    Dp(1.2f));
        ImGui::Dummy({0, FontSize(fonts.caption) + Dp(10)});
        const BuildIdentity self = updates_->Build();
        const PortVersion mine = ParsePortVersion(self.version);
        const float row_width = width - Dp(20), row_height = Dp(56);
        int choice = 0;
        for (const auto& build : update_.test_builds)
        {
            const bool followed = build.pull_request == update_.track;
            const bool running = self.origin == BuildOrigin::Test && build.pull_request == self.pull_request;
            const bool current = running && !mine.commit.empty() && build.version.commit.rfind(mine.commit, 0) == 0;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::PushID(build.pull_request);
            if (ImGui::InvisibleButton("##test_build", {row_width, row_height})) choice = build.pull_request;
            if (followed) ImGui::SetItemDefaultFocus();
            const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused(), 14.0f);
            ImGui::PopID();
            const ImVec2 max{pos.x + row_width, pos.y + row_height};
            if (followed || hover > 0.01f)
                draw->AddRectFilled(pos, max,
                                    Col(Mix(DialogFill(), color::warning, followed ? 0.10f + 0.06f * hover : 0.07f),
                                        followed ? 1.0f : hover),
                                    Dp(10));
            // A flask in a soft disc, then the version and title over the pull request and date.
            const ImVec2 icon{pos.x + Dp(26), pos.y + row_height * 0.5f};
            draw->AddCircleFilled(icon, Dp(15), Col(Mix(DialogFill(), color::warning, followed ? 0.24f : 0.13f)));
            DrawIcon(draw, Icon::Flask, icon, Dp(15), Col(color::warning, followed ? 1.0f : 0.8f));
            const float text_left = pos.x + Dp(52), text_right = max.x - Dp(36);
            const std::string number = TestBuildNumber(build);
            const float line1 = pos.y + Dp(9);
            DrawLabel(draw, fonts.label, {text_left, line1}, Col(color::text), number.c_str());
            const float title_left = text_left + TextSize(fonts.label, number.c_str()).x + Dp(9);
            const std::string subtitle = Elide(fonts.body, TestBuildSubtitle(build), text_right - title_left);
            DrawLabel(draw, fonts.body,
                      {title_left, line1 + (FontSize(fonts.label) - FontSize(fonts.body)) * 0.5f + Dp(1)},
                      Col(Mix(color::muted, color::text, 0.55f + 0.45f * hover)), subtitle.c_str());
            std::string detail = "Test build #" + std::to_string(build.pull_request);
            if (build.published) detail += "  \xC2\xB7  " + LocalTime(build.published);
            if (build.hidden) detail += "  \xC2\xB7  hidden";
            const float line2 = pos.y + row_height - Dp(9) - FontSize(fonts.caption);
            DrawLabel(draw, fonts.caption, {text_left, line2}, Col(color::dim), detail.c_str());
            // Installed, or the running build's pull request has a newer build.
            if (running)
            {
                const char* tag = current ? "INSTALLED" : "UPDATE";
                const ImVec4 tint = current ? color::muted : color::accent;
                const float tag_x = text_left + TextSize(fonts.caption, detail.c_str()).x + Dp(10);
                DrawTracked(draw, fonts.caption, {tag_x, line2}, Col(tint), tag, Dp(0.8f));
            }
            if (followed) DrawIcon(draw, Icon::Check, {max.x - Dp(18), pos.y + row_height * 0.5f}, Dp(15), Col(color::warning));
            ImGui::Dummy({0, Dp(2)});
        }
        ImGui::Dummy({0, Dp(8)});
        TextWrappedColored(fonts.caption, color::dim, "Updates come from the test build you choose. Release takes you "
                                                      "back to the official release.");
        ImGui::PopStyleVar();
        if (choice)
        {
            ImGui::CloseCurrentPopup();
            test_menu_chose_ = true;
            SelectTestBuild(choice);
        }
        else if (CancelPressed()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // One line on the Advanced page: what the last check found.
    std::string UpdateSummary() const
    {
        using State = UpdateService::State;
        const std::string checked = update_.checked ? " " + LastChecked() : std::string();
        switch (update_.state)
        {
        case State::Idle: return "Not checked yet.";
        case State::Checking: return "Checking...";
        case State::UpToDate:
            if (ChooseTestBuild()) return "Test builds are available; choose one with Test on the Play page." + checked;
            return "This build is up to date." + checked;
        case State::CheckFailed: return "Couldn't check: " + update_.error;
        case State::Available:
            switch (ClassifyOffer(updates_->Build(), *update_.offer))
            {
            case OfferKind::Update:
                return (update_.offer->test ? std::string("A new test build")
                                            : "Version " + VersionNumber(update_.offer->version))
                    + " is available." + checked;
            case OfferKind::TestBuild: return "A test build is available." + checked;
            case OfferKind::BackToRelease:
                return "Release " + VersionNumber(update_.offer->version) + " is available to go back to." + checked;
            }
            break;
        case State::Downloading:
        case State::Installing:
        case State::Installed: return "Installing an update...";
        case State::InstallFailed: return "The update failed: " + update_.error;
        }
        return {};
    }

    // What the offered build changes, and its download and installation: a
    // card over the dimmed launcher. Where the launcher fills the screen
    // (Steam's Game Mode) the card keeps its size.
    void DrawUpdateDialog()
    {
        if (update_dialog_)
        {
            ImGui::OpenPopup("Update");
            update_dialog_ = false;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(Dp(640), viewport->Size.x - Dp(48));
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({width, 0});
        // The contents draw the card and its shadow; the window itself is clear.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(30, 26));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0, 0, 0, 0});
        if (ImGui::BeginPopupModal("Update", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Dp(10), 0.0f});
            if (update_.offer) DrawUpdateDialogContents(*update_.offer);
            else ImGui::CloseCurrentPopup();
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    static ImVec4 DialogFill() { return Mix(color::card, color::field, 0.35f); }

    // The dialog's card: a soft shadow, a fill a shade lighter than the
    // launcher's cards, a hairline edge, and the offer's colour as a fine line
    // along the top that fades out to both sides.
    static void DrawDialogCard(const ImVec4& tint, float rounding = Dp(18), float drop = Dp(14))
    {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetWindowPos();
        const ImVec2 max{min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight()};
        draw->PushClipRectFullScreen();
        for (int i = 0; i < 8; ++i)
        {
            const float grow = Dp(2.0f + 3.0f * float(i));
            draw->AddRectFilled({min.x - grow, min.y - grow + drop}, {max.x + grow, max.y + grow + drop},
                                Col(ImVec4{0, 0, 0, 1}, 0.075f - 0.008f * float(i)), rounding + grow);
        }
        draw->AddRectFilled(min, max, Col(DialogFill()), rounding);
        draw->AddRect(min, max, Col(Mix(color::border, color::muted, 0.15f), 0.75f), rounding, 0,
                      std::max(1.0f, Dp(1)));
        const float mid = (min.x + max.x) * 0.5f, line = std::max(1.0f, Dp(1.5f));
        const ImU32 clear = Col(tint, 0.0f), strong = Col(tint, 0.9f);
        draw->AddRectFilledMultiColor({min.x + rounding, min.y}, {mid, min.y + line}, clear, strong, strong, clear);
        draw->AddRectFilledMultiColor({mid, min.y}, {max.x - rounding, min.y + line}, strong, clear, clear, strong);
        draw->PopClipRect();
    }

    // A hairline across the whole card at the cursor.
    static void DialogDivider()
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float left = ImGui::GetWindowPos().x, right = left + ImGui::GetWindowWidth();
        const float thickness = std::max(1.0f, Dp(1));
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRectFullScreen();
        draw->AddRectFilled({left + thickness, at.y}, {right - thickness, at.y + thickness}, Col(color::border, 0.8f));
        draw->PopClipRect();
        ImGui::Dummy({1.0f, thickness});
    }

    // A release's title without what the headline already says: CI names
    // releases "Recharged <version>" (build.yml), before the rename (#64)
    // "Mario Strikers Charged Native Port <version>".
    static std::string ReleaseSubtitle(std::string title, const PortVersion& version)
    {
        const std::string number = VersionNumber(version);
        for (const std::string& part : {"Recharged v" + number, "Recharged " + number, "v" + number, number,
                                        std::string("Mario Strikers Charged Native Port")})
            if (const auto at = title.find(part); at != std::string::npos) title.erase(at, part.size());
        const auto separator = [](char c) { return c == ' ' || c == '-' || c == ':' || c == '|'; };
        while (!title.empty() && separator(title.front())) title.erase(title.begin());
        while (!title.empty() && separator(title.back())) title.pop_back();
        // An en dash ("1.0.11 – Mirror matches") leaves its UTF-8 bytes in front.
        if (title.rfind("\xE2\x80\x93", 0) == 0) title = ReleaseSubtitle(title.substr(3), version);
        return title;
    }

    static float PrimaryWidth(const char* label, Icon icon)
    {
        ImFont* font = CurrentFonts().label;
        return TextSize(font, label).x + (icon == Icon::None ? 0.0f : FontSize(font) * 0.82f + Dp(10)) + Dp(44);
    }

    static float GhostWidth(const char* label, Icon icon)
    {
        return TextSize(CurrentFonts().body, label).x + (icon == Icon::None ? 0.0f : Dp(24)) + Dp(28);
    }

    // Escape, or B on a gamepad.
    static bool CancelPressed()
    {
        return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    }

    void DrawUpdateDialogContents(const PublishedBuild& offer)
    {
        using State = UpdateService::State;
        const auto& fonts = CurrentFonts();
        const auto& status = update_;
        const BuildIdentity self = updates_->Build();
        const OfferKind kind = ClassifyOffer(self, offer);
        const ImVec4 tint = OfferTint(kind, offer);
        const PackageAsset* asset = offer.PackageFor(self.package);
        auto* draw = ImGui::GetWindowDrawList();
        DrawDialogCard(tint);
        const float right_edge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;

        // Header: the offer's icon in a soft halo, what kind of build it is, which one and its title.
        const char* overline = "";
        std::string headline;
        switch (kind)
        {
        case OfferKind::Update:
            overline = offer.test ? "NEW TEST BUILD" : "UPDATE AVAILABLE";
            headline = offer.test ? "Pull request #" + std::to_string(offer.pull_request)
                                  : "Version " + VersionNumber(offer.version);
            break;
        case OfferKind::TestBuild:
            overline = "INSTALL TEST BUILD";
            headline = "Pull request #" + std::to_string(offer.pull_request);
            break;
        case OfferKind::BackToRelease:
            overline = "INSTALL RELEASE";
            headline = "Version " + VersionNumber(offer.version);
            break;
        }
        const std::string title = offer.test ? (status.pull && !status.pull->title.empty() ? status.pull->title
                                                                                           : offer.title)
                                             : ReleaseSubtitle(offer.title, offer.version);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float disc = Dp(46);
        const ImVec2 center{top.x + disc * 0.5f, top.y + Dp(2) + disc * 0.5f};
        for (int i = 3; i >= 1; --i)
            draw->AddCircleFilled(center, disc * 0.5f + Dp(6.0f * float(i)), Col(tint, 0.025f));
        draw->AddCircleFilled(center, disc * 0.5f, Col(Mix(DialogFill(), tint, 0.16f)));
        DrawIcon(draw, OfferIcon(kind, offer), center, Dp(21), Col(tint));
        const float column = top.x + disc + Dp(20);
        ImGui::SetCursorScreenPos({column, top.y});
        ImGui::BeginGroup();
        DrawTracked(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(tint, 0.95f), overline, Dp(1.4f));
        ImGui::Dummy({0, FontSize(fonts.caption) + Dp(5)});
        TextColored(fonts.heading, color::text, headline.c_str());
        if (!title.empty())
        {
            ImGui::Dummy({0, Dp(3)});
            TextWrappedColored(fonts.body, Mix(color::muted, color::text, 0.45f), title.c_str());
        }
        ImGui::Dummy({0, Dp(10)});

        // From the running build to the offered one (equal numbers differ in
        // their commits), then the date and the download's size.
        const PortVersion mine = ParsePortVersion(self.version);
        const bool same_number = CompareVersions(mine, offer.version) == 0;
        auto describe = [same_number](const PortVersion& version, const std::string& what) {
            std::string text = VersionNumber(version);
            if (same_number && !version.commit.empty()) text += " (" + version.commit.substr(0, 8) + ")";
            return text + "  \xC2\xB7  " + what;
        };
        const std::string mine_what = self.origin == BuildOrigin::Release ? "Release"
            : self.origin == BuildOrigin::Test ? "Test build #" + std::to_string(self.pull_request)
                                               : "Local build";
        const std::string from = describe(mine, mine_what);
        const std::string titled = offer.test ? TitleVersion(offer) : std::string();
        const std::string to = !titled.empty()
            ? titled + "  \xC2\xB7  Test build #" + std::to_string(offer.pull_request)
            : describe(offer.version, offer.test ? "Test build #" + std::to_string(offer.pull_request) : "Release");
        const float caption = FontSize(fonts.caption);
        TextColored(fonts.caption, color::muted, from.c_str());
        ImGui::SameLine(0, Dp(10));
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy({Dp(8), caption});
            DrawChevron(draw, {at.x + Dp(4), at.y + caption * 0.55f}, Dp(10), Col(color::dim));
        }
        ImGui::SameLine(0, Dp(10));
        TextColored(fonts.caption, Mix(color::text, tint, 0.3f), to.c_str());
        std::string meta;
        if (offer.published) meta = LocalTime(offer.published);
        if (asset && asset->size) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + Megabytes(asset->size);
        if (!meta.empty())
        {
            const float meta_width = TextSize(fonts.caption, meta.c_str()).x;
            ImGui::SameLine(0, 0);
            if (ImGui::GetCursorScreenPos().x + Dp(20) + meta_width > right_edge)
            {
                ImGui::NewLine();
                ImGui::Dummy({0, Dp(3)});
            }
            else ImGui::SetCursorScreenPos({right_edge - meta_width, ImGui::GetCursorScreenPos().y});
            TextColored(fonts.caption, color::dim, meta.c_str());
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos({top.x, std::max(top.y + disc + Dp(4), ImGui::GetItemRectMax().y)});
        ImGui::Dummy({0, Dp(20)});

        // A switch between test builds and releases says what it means.
        bool callout = false;
        if (kind != OfferKind::Update)
        {
            std::string meaning;
            if (kind == OfferKind::TestBuild && self.origin == BuildOrigin::Test)
                meaning = "This replaces test build #" + std::to_string(self.pull_request) + " by test build #"
                    + std::to_string(offer.pull_request)
                    + ", whose updates you then get. Test on the Play page switches between test builds, Release "
                      "takes you back to the official release.";
            else if (kind == OfferKind::TestBuild)
                meaning = "Test builds have changes that are not released yet, to try them early; they can have new "
                          "problems. Release on the Play page takes you back.";
            else
                meaning = "This replaces your test build by the official release: changes that are not released yet "
                          "go away. Your settings and saves stay.";
            if (kind == OfferKind::BackToRelease && !UpdatesItself(offer.version))
                meaning += " Version " + VersionNumber(offer.version)
                    + " cannot update itself; newer versions are downloaded from the releases page.";
            Callout(meaning, tint, kind == OfferKind::TestBuild ? Icon::Warning : Icon::Info);
            ImGui::Dummy({0, Dp(20)});
            callout = true;
        }

        // The release notes, or the test build's pull request description:
        // no frame, the text fades out where it scrolls.
        DrawTracked(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(color::dim),
                    offer.test ? "PULL REQUEST" : "RELEASE NOTES", Dp(1.2f));
        ImGui::Dummy({0, caption + Dp(10)});
        const std::string& source = offer.test && status.pull ? status.pull->body : offer.notes;
        if (source != notes_source_)
        {
            notes_source_ = source;
            notes_ = ParseReleaseNotes(source);
        }
        float notes_height = ImGui::GetMainViewport()->Size.y * 0.36f;
        if (callout) notes_height -= Dp(64);
        if (status.state == State::InstallFailed || !status.install_blocker.empty()) notes_height -= Dp(56);
        ImGui::SetNextWindowSizeConstraints({0, Dp(24)}, {FLT_MAX, std::clamp(notes_height, Dp(90), Dp(340))});
        ImGui::BeginChild("##notes", {ImGui::GetContentRegionAvail().x, 0}, ImGuiChildFlags_AutoResizeY);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Dp(10), 0.0f});
            const float text_width = ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0 ? Dp(10) : 0);
            if (notes_.empty())
                TextWrappedColored(fonts.body, color::muted, "No notes were published for this build.");
            else DrawReleaseNotes(notes_, text_width);
            ImGui::PopStyleVar();
            // The right stick scrolls the notes.
            const float stick = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue
                - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
            if (std::fabs(stick) > 0.15f)
                ImGui::SetScrollY(ImGui::GetScrollY() + stick * Dp(700) * ImGui::GetIO().DeltaTime);
            const float scroll = ImGui::GetScrollY(), scroll_max = ImGui::GetScrollMaxY();
            if (scroll_max > 0.0f)
            {
                auto* notes_draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                const float fade = Dp(30), right = at.x + size.x - ImGui::GetStyle().ScrollbarSize;
                const ImU32 solid = Col(DialogFill()), clear = Col(DialogFill(), 0.0f);
                if (scroll < scroll_max - 0.5f)
                    notes_draw->AddRectFilledMultiColor({at.x, at.y + size.y - fade}, {right, at.y + size.y}, clear,
                                                        clear, solid, solid);
                if (scroll > 0.5f)
                    notes_draw->AddRectFilledMultiColor(at, {right, at.y + fade}, solid, solid, clear, clear);
            }
        }
        ImGui::EndChild();
        ImGui::Dummy({0, Dp(18)});

        // Progress or problems, above the footer.
        const bool busy = status.state == State::Downloading || status.state == State::Installing
            || status.state == State::Installed;
        if (status.state == State::Downloading)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float bar = ImGui::GetContentRegionAvail().x;
            const float fraction = status.total ? std::min(1.0f, float(double(status.received) / double(status.total)))
                                                : 0.0f;
            draw->AddRectFilled(pos, {pos.x + bar, pos.y + Dp(4)}, Col(color::field), Dp(2));
            if (fraction > 0.0f)
                FillGradient(draw, pos, {pos.x + std::max(Dp(4), bar * fraction), pos.y + Dp(4)},
                             Col(Mix(color::accent_dark, color::accent, 0.55f)), Col(color::accent), Dp(2), true);
            ImGui::Dummy({bar, Dp(4)});
            ImGui::Dummy({0, Dp(8)});
            const std::string progress = "Downloading  \xC2\xB7  " + Megabytes(status.received)
                + (status.total ? " of " + Megabytes(status.total) : std::string());
            const float line = ImGui::GetCursorScreenPos().y;
            TextColored(fonts.caption, color::muted, progress.c_str());
            if (status.total)
            {
                const std::string percent = std::to_string(status.received * 100 / status.total) + "%";
                DrawLabel(draw, fonts.caption, {pos.x + bar - TextSize(fonts.caption, percent.c_str()).x, line},
                          Col(color::text), percent.c_str());
            }
            ImGui::Dummy({0, Dp(18)});
        }
        else if (status.state == State::InstallFailed)
        {
            Callout(status.error, color::danger, Icon::Error);
            ImGui::Dummy({0, Dp(18)});
        }
        else if (!status.install_blocker.empty() && !busy)
        {
            Callout(status.install_blocker + " Download the package and unpack it over this folder instead.",
                    color::muted, Icon::Info);
            ImGui::Dummy({0, Dp(18)});
        }
        const bool ask_to_save = status.state == State::Installed && dirty_;
        if (ask_to_save)
        {
            // The launcher restarts into the new version; settings first.
            TextWrappedColored(fonts.body, color::text, "Save your settings changes before the launcher restarts?");
            ImGui::Dummy({0, Dp(18)});
        }

        // Footer: the build's page on GitHub on the left, the actions on the right.
        DialogDivider();
        ImGui::Dummy({0, Dp(18)});
        const float row = ImGui::GetCursorScreenPos().y, height = Dp(40);
        auto align_right = [&](float width) { ImGui::SetCursorScreenPos({right_edge - width, row}); };
        if (ask_to_save)
        {
            const char *discard = "Discard and restart", *save = "Save and restart";
            align_right(GhostWidth(discard, Icon::None) + Dp(10) + PrimaryWidth(save, Icon::None));
            if (GhostButton("update_discard_restart", discard, {GhostWidth(discard, Icon::None), height})) Reload();
            ImGui::SameLine(0, Dp(10));
            if (PrimaryButton("update_save_restart", save, {PrimaryWidth(save, Icon::None), height}, true)) Save();
            ImGui::SetItemDefaultFocus();
        }
        else if (status.state == State::Downloading)
        {
            const char* cancel = "Cancel download";
            align_right(GhostWidth(cancel, Icon::None));
            if (GhostButton("update_cancel", cancel, {GhostWidth(cancel, Icon::None), height})) updates_->Cancel();
        }
        else if (busy)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            Spinner({pos.x + Dp(8), pos.y + height * 0.5f}, Dp(7), Col(tint));
            DrawLabel(draw, fonts.body, {pos.x + Dp(26), pos.y + (height - FontSize(fonts.body)) * 0.5f},
                      Col(color::text), status.state == State::Installing ? "Installing..." : "Restarting...");
            ImGui::Dummy({1.0f, height});
        }
        else
        {
            const std::string page = offer.test && status.pull && !status.pull->page.empty() ? status.pull->page
                                                                                             : offer.page;
            const char* github = "View on GitHub";
            if (!page.empty())
            {
                if (GhostButton("update_github", github, {GhostWidth(github, Icon::GitHub), height}, true,
                                Icon::GitHub))
                    SDL_OpenURL(page.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", page.c_str());
            }
            const char* action = nullptr;
            if (status.install_blocker.empty())
                action = status.state == State::InstallFailed ? "Try again"
                    : kind == OfferKind::Update               ? "Update now"
                                                              : "Switch now";
            else if (asset) action = "Download";
            const char* later = "Later";
            const float later_width = GhostWidth(later, Icon::None);
            const float action_width = action ? PrimaryWidth(action, Icon::Download) : 0.0f;
            align_right(later_width + (action ? Dp(10) + action_width : 0.0f));
            if (GhostButton("update_later", later, {later_width, height}) || CancelPressed())
                DismissUpdateDialog();
            if (action)
            {
                ImGui::SameLine(0, Dp(10));
                if (PrimaryButton("update_now", action, {action_width, height}, true, Icon::Download))
                {
                    if (status.install_blocker.empty())
                    {
                        // Its pull request is followed from now on, also after it is released.
                        if (offer.test && draft_.test_build != offer.pull_request)
                            SaveUpdateChoice(draft_.update_channel, offer.pull_request);
                        updates_->Install();
                    }
                    else SDL_OpenURL(asset->url.c_str());
                }
                if (ImGui::IsItemHovered() && status.install_blocker.empty())
                    ImGui::SetTooltip("Downloads %s, replaces the program files and restarts the launcher.\n"
                                      "Your settings, saves and logs stay.",
                                      asset ? asset->name.c_str() : "the package");
                ImGui::SetItemDefaultFocus();
            }
        }
    }

    // A path as the settings keep it: relative inside the settings folder (it
    // moves along with it), absolute elsewhere; links stay as they are.
    std::string SettingsPath(const fs::path& path) const
    {
        std::error_code error;
        const auto absolute = fs::absolute(path, error).lexically_normal();
        const auto folder = fs::absolute(file_.path, error).parent_path().lexically_normal();
        const auto relative = absolute.lexically_relative(folder);
        const bool inside = !error && !relative.empty() && *relative.begin() != "..";
        return PathUtf8(inside ? relative : absolute);
    }

    // A file chosen with Choose file or Locate, or dropped onto the window: an
    // image is checked and chosen only when this build plays it; a dropped
    // folder becomes the game folder.
    void SelectDisc(const std::string& path)
    {
        std::error_code error;
        if (fs::is_directory(PathFromUtf8(path), error))
        {
            SelectGameFolder(PathFromUtf8(path));
            return;
        }
        if (options_.launch.disc)
        {
            Notice("The image from the command line (--disc) is used for this run.", NoticeKind::Info);
            return;
        }
        candidate_pending_ = true;
        candidate_ = std::async(std::launch::async, [path, known = KnownBanners()] {
            return RunDiscCheck(PathFromUtf8(path), path, known);
        });
    }

    void CandidateChecked(DiscCheck result)
    {
        const auto path = PathFromUtf8(result.selection);
        const std::string name = PathUtf8(path.filename());
        KeepCheck(path, result);
        if (result.info && SupportedRelease(*result.info)) UseImage(path, *result.info, result.size);
        else if (result.info)
        {
            // Recognised, but this build cannot run it: the chosen image stays.
            region_prompt_info_ = *result.info;
            region_prompt_ = true;
            Notice(name + ": " + UnplayableReason(*result.info), NoticeKind::Warning);
        }
        else Notice(name + " cannot be used: " + result.error, NoticeKind::Error);
    }

    // Makes this playable image the one Play starts. Only an explicit choice
    // pins an image: scans, sorting and searches never do.
    void Pin(const fs::path& path, const DiscInfo& info, std::uintmax_t size = 0)
    {
        if (options_.launch.disc)
        {
            Notice("The image from the command line (--disc) is used for this run.", NoticeKind::Info);
            return;
        }
        draft_.disc = SettingsPath(path);
        dirty_ = true;
        check_ = {};
        check_.info = info;
        check_.size = size;
        check_.selection = DiscSelection();
    }

    // Remembers what a check found (the cache keeps a pinned image's release
    // for when it goes missing) and the banner it read.
    void KeepCheck(const fs::path& path, DiscCheck& result)
    {
        if (result.info)
        {
            DiscProbe probe;
            probe.kind = DiscProbe::Kind::Charged;
            probe.info = *result.info;
            image_cache_.Put(path, result.size, result.modified, probe);
            SaveImageCache(image_cache_, library_data_ / "library.json");
            if (!result.banner.empty()) StoreBanner(result.info->game_id, result.banner);
            else if (result.banner_read) banner_failures_.insert(result.info->game_id);
        }
    }

    void CheckDisc()
    {
        try
        {
            const auto launch = EffectiveLaunch();
            const auto path = launch.disc_path;
            if (path.empty())
            {
                // Nothing chosen yet is not a read error; the card asks for a disc.
                pending_ = false;
                check_ = {};
                return;
            }
            const auto selection = PathUtf8(path);
            pending_ = true;
            check_ = {};
            future_ = std::async(std::launch::async, [path, selection, known = KnownBanners()] {
                return RunDiscCheck(path, selection, known);
            });
        }
        catch (const std::exception& e) { pending_ = false; check_.error = e.what(); }
    }

    // Nothing chosen yet (a first start): the game folder's scan chooses its
    // first playable image (AutoChoose); without one, the usual places are
    // searched. A chosen image that has gone is never replaced without asking.
    void StartDiscSearch()
    {
        if (!config_ok_ || options_.launch.disc || options_.smoke_test || !draft_.disc.empty()) return;
        autochoose_ = AutoChoice::FirstStart;
    }

    // Looks for the game in the settings folder, Games, Emulation/roms/wii,
    // Downloads and removable drives, in the background (each image is opened
    // to check it).
    void SearchUsualPlaces()
    {
        if (searching_) return;
        std::error_code error;
        const auto user_folder = [](SDL_Folder which) {
            const char* path = SDL_GetUserFolder(which);
            return path ? PathFromUtf8(path) : fs::path();
        };
        const auto settings = fs::absolute(file_.path, error).parent_path();
        const auto home = user_folder(SDL_FOLDER_HOME), downloads = user_folder(SDL_FOLDER_DOWNLOADS);
        search_from_ = draft_.disc;
        searching_ = true;
        search_ = std::async(std::launch::async, [settings, home, downloads] {
            try
            {
                std::vector<fs::path> drives;
#if defined(__linux__)
                const char* user = std::getenv("USER");
                drives = launcher::RemovableDrives("/run/media", user ? user : "");
#endif
                const auto folders = launcher::DiscSearchFolders(settings, home, downloads, drives);
                return launcher::PickDisc(launcher::FindDiscImages(folders),
                                          [](const fs::path& path) { return InspectDisc(path); });
            }
            catch (const std::exception&) { return launcher::DiscSearchResult{}; }
        });
    }

    void DiscSearchDone(const launcher::DiscSearchResult& result)
    {
        // A disc the player chose meanwhile stays.
        if (draft_.disc != search_from_ || options_.launch.disc) return;
        const bool clean = !dirty_;
        // A game folder without the game takes the folder it was found in.
        std::string moved;
        const bool folder_has_game = std::any_of(library_.begin(), library_.end(), [](const LibraryImage& image) {
            return image.probe.kind == DiscProbe::Kind::Charged;
        });
        const fs::path found = !result.disc.empty() ? result.disc : result.charged.empty() ? fs::path()
                                                                                           : result.charged.front();
        if (!found.empty() && !folder_has_game)
        {
            draft_.folder = SettingsPath(found.parent_path());
            dirty_ = true;
            moved = " Its folder is now your game folder.";
        }
        if (result.disc.empty())
        {
            if (!result.other.empty())
            {
                const DiscInfo other{"", result.other_id, "", result.other_revision, 0};
                Notice("Found Mario Strikers Charged at " + PathUtf8(result.other) + ". " + UnplayableReason(other)
                           + " This build runs " + PlayableReleasesText() + "." + moved,
                       NoticeKind::Warning);
            }
            if (clean && !moved.empty()) Save();
            return;
        }
        draft_.disc = SettingsPath(result.disc);
        dirty_ = true;
        check_ = {};
        if (!pending_) CheckDisc();
        std::string text = "Found your game: " + PathUtf8(result.disc.filename()) + "." + moved;
        if (result.supported > 1) text += " There are more; choose another on the Game page.";
        // Nothing else changed yet: keep it, so Play starts without asking first.
        if (clean) Save();
        Notice(text, NoticeKind::Success);
    }

    // ------------------------------------------------------- image library
    LibraryFolder GameFolder() const
    {
        auto path = ResolveGameFolder(draft_, file_.path).lexically_normal();
        if (path.filename().empty() && path.has_parent_path()) path = path.parent_path(); // "folder/" or "folder/."
        return {path, draft_.folder_subfolders};
    }

    void SelectGameFolder(const fs::path& folder)
    {
        const auto before = GameFolder();
        draft_.folder = SettingsPath(folder);
        Notice("Your game folder is now " + PathUtf8(folder) + ".", NoticeKind::Info);
        GameFolderChanged(before);
    }

    // The player selected a game folder (the same one too) or switched
    // Subfolders: the folder is read again from scratch. An image chosen in
    // the old folder that the new rules leave out gives way to the new
    // folder's first playable image, or to none; a file added with Choose file
    // (outside the old folder) stays chosen.
    void GameFolderChanged(const LibraryFolder& before)
    {
        dirty_ = true;
        const auto chosen = PathFromUtf8(DiscSelection());
        if (!options_.launch.disc && !chosen.empty() && InFolder(chosen, before) && !InFolder(chosen, GameFolder()))
        {
            draft_.disc.clear();
            check_ = {};
        }
        if (!options_.launch.disc && draft_.disc.empty()) autochoose_ = AutoChoice::NewFolder;
        StartLibraryScan();
    }

    // Reads the game folder again from scratch: the list is empty until the
    // scan has opened every image. A scan already running is told to stop,
    // and the new one follows it.
    void StartLibraryScan()
    {
        scanned_folder_ = draft_.folder;
        scanned_subfolders_ = draft_.folder_subfolders;
        library_ = {};
        library_available_ = {};
        library_other_games_ = 0;
        library_limited_ = false;
        if (library_scanning_)
        {
            library_rescan_ = true;
            library_progress_->cancel = true;
            return;
        }
        library_rescan_ = false;
        const std::vector<LibraryFolder> folders{GameFolder()};
        std::vector<fs::path> keep;
        if (!DiscSelection().empty()) keep.push_back(PathFromUtf8(DiscSelection()));
        library_progress_ = std::make_shared<ScanProgress>();
        library_scanning_ = true;
        library_scan_folders_ = folders;
        library_scan_ = std::async(std::launch::async, [folders, cache = image_cache_, known = KnownBanners(), keep,
                                                        progress = library_progress_] {
            try
            {
                return ScanLibrary(folders, cache, known, keep, *progress, ProbeDisc, ReadReleaseBanner);
            }
            catch (const std::exception&)
            {
                LibraryScan failed;
                failed.cancelled = true;
                return failed;
            }
        });
    }

    void LibraryScanDone(LibraryScan scan)
    {
        library_scanning_ = false;
        if (!scan.cancelled)
        {
            // Keep what the pinned image's own check found meanwhile.
            const auto pinned_path = PathFromUtf8(DiscSelection());
            std::optional<ImageCache::Entry> pinned;
            if (!pinned_path.empty())
                if (const auto* entry = image_cache_.Last(pinned_path)) pinned = *entry;
            image_cache_ = std::move(scan.cache);
            if (pinned && !image_cache_.Last(pinned_path))
                image_cache_.Put(pinned_path, pinned->size, pinned->modified, pinned->probe);
            SaveImageCache(image_cache_, library_data_ / "library.json");
            library_ = std::move(scan.images);
            library_available_ = std::move(scan.available);
            library_folders_ = library_scan_folders_;
            library_other_games_ = scan.other_games;
            library_strikers_ = std::move(scan.strikers);
            PrefillGameCubeDisc();
            library_limited_ = scan.limited;
            for (auto& [id, rgba] : scan.banners) StoreBanner(id, rgba);
            banner_failures_.insert(scan.banner_failures.begin(), scan.banner_failures.end());
            if (autochoose_ != AutoChoice::None && !library_rescan_) AutoChoose();
        }
        if (library_rescan_) StartLibraryScan();
    }

    // Nothing chosen (a first start, or a new folder): the first playable
    // image of the game folder, in release order. Without one, a first start
    // searches the usual places; a new folder leaves nothing chosen.
    void AutoChoose()
    {
        const auto reason = autochoose_;
        autochoose_ = AutoChoice::None;
        if (!draft_.disc.empty() || options_.launch.disc) return;
        for (const auto* image : ArrangeImages(library_, LibrarySort::Release, false))
            if (StateOf(*image) == ImageState::Playable)
            {
                const bool clean = !dirty_;
                Pin(image->path, image->probe.info, image->size);
                if (clean) Save();
                Notice("Play starts " + PathUtf8(image->path.filename()) + " (" + RegionRevision(image->probe.info)
                           + ") from your game folder.",
                       NoticeKind::Success);
                return;
            }
        if (reason == AutoChoice::FirstStart) SearchUsualPlaces();
        else
            Notice("No playable Mario Strikers Charged image in this folder. Select another folder or choose a file.",
                   NoticeKind::Warning);
    }

    // Once a frame: scan when the game folder changes or when it was
    // not available comes back, and notice when a missing pinned image comes
    // back (a drive plugged in again).
    void SyncLibrary()
    {
        if (!config_ok_) return;
        if (draft_.folder != scanned_folder_ || draft_.folder_subfolders != scanned_subfolders_) StartLibraryScan();
        if (!library_scanning_ && SDL_GetTicks() - folders_probe_ > 3000)
        {
            folders_probe_ = SDL_GetTicks();
            for (std::size_t n = 0; n < library_available_.size() && n < library_folders_.size(); ++n)
            {
                std::error_code error;
                if (!library_available_[n] && fs::is_directory(library_folders_[n].path, error))
                {
                    StartLibraryScan();
                    break;
                }
            }
        }
        if (!pending_ && !check_.missing.empty() && check_.selection == DiscSelection()
            && SDL_GetTicks() - missing_probe_ > 2000)
        {
            missing_probe_ = SDL_GetTicks();
            std::error_code error;
            if (fs::exists(PathFromUtf8(check_.selection), error)) CheckDisc();
        }
    }

    // ------------------------------------------------------------ banners
    fs::path BannerFile(const std::string& id) const { return library_data_ / "banners" / (id + ".png"); }

    // Disc IDs whose banner is cached, or could not be read in this session.
    std::set<std::string> KnownBanners() const
    {
        std::set<std::string> ids = banner_failures_;
        for (const auto& [id, texture] : banners_)
            if (texture) ids.insert(id);
        std::error_code error;
        for (fs::directory_iterator it(library_data_ / "banners", error), end; !error && it != end; it.increment(error))
            if (it->path().extension() == ".png") ids.insert(PathUtf8(it->path().stem()));
        return ids;
    }

    void SetBannerTexture(const std::string& id, SDL_Surface* surface)
    {
        auto& slot = banners_[id];
        if (slot) SDL_DestroyTexture(slot);
        slot = SDL_CreateTextureFromSurface(renderer_, surface);
        if (slot) SDL_SetTextureScaleMode(slot, SDL_SCALEMODE_LINEAR);
    }

    // Caches a banner read from a player's image (data/launcher/banners).
    void StoreBanner(const std::string& id, std::vector<std::uint8_t>& rgba)
    {
        if (!SafeGameId(id) || rgba.size() != std::size_t(kBannerWidth) * kBannerHeight * 4) return;
        banner_missing_.erase(id);
        auto* surface = SDL_CreateSurfaceFrom(kBannerWidth, kBannerHeight, SDL_PIXELFORMAT_RGBA32, rgba.data(),
                                              kBannerWidth * 4);
        if (!surface) return;
        std::error_code error;
        fs::create_directories(BannerFile(id).parent_path(), error);
        SDL_SavePNG(surface, PathUtf8(BannerFile(id)).c_str());
        if (renderer_) SetBannerTexture(id, surface);
        SDL_DestroySurface(surface);
    }

    // A release's banner: from the cache folder the first time it is drawn;
    // null while there is none.
    SDL_Texture* Banner(const std::string& id)
    {
        if (!SafeGameId(id)) return nullptr;
        if (const auto it = banners_.find(id); it != banners_.end() && it->second) return it->second;
        if (banner_missing_.count(id)) return nullptr;
        if (auto* surface = SDL_LoadPNG(PathUtf8(BannerFile(id)).c_str()))
        {
            if (surface->w == kBannerWidth && surface->h == kBannerHeight) SetBannerTexture(id, surface);
            SDL_DestroySurface(surface);
        }
        const auto it = banners_.find(id);
        if (it == banners_.end() || !it->second)
        {
            banner_missing_.insert(id); // until a scan or check reads it
            return nullptr;
        }
        return it->second;
    }

    // The banner of a release in [min, max] (3:1), or a placeholder with a disc.
    void DrawBanner(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& id, float alpha = 1.0f)
    {
        const float rounding = (max.y - min.y) * 0.14f;
        if (auto* texture = Banner(id))
        {
            draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)), min, max,
                                  {0, 0}, {1, 1}, Col({1, 1, 1, 1}, alpha), rounding);
            return;
        }
        FillGradient(draw, min, max, Col(Mix(color::field, color::accent, 0.12f), alpha), Col(color::field, alpha),
                     rounding, true);
        draw->AddRect(min, max, Col(color::border, alpha), rounding, 0, Dp(1));
        DrawIcon(draw, Icon::Disc, {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}, (max.y - min.y) * 0.46f,
                 Col(color::dim, alpha));
    }

    void Browse(const fs::path& start = {})
    {
        // "All files" too: a system that does not know the .rvz type (macOS) may
        // grey such files out under the first filter. The check reads the content.
        static const SDL_DialogFileFilter filters[] = {
            {"Wii disc images (ISO / RVZ / WBFS)", "iso;rvz;wbfs;ISO;RVZ;WBFS"}, {"All files", "*"}};
        {
            std::lock_guard<std::mutex> lock(dialog_->mutex);
            dialog_->active = true;
        }
        const std::string location = start.empty() ? std::string() : PathUtf8(start);
        SDL_ShowOpenFileDialog(DiscChosen, new std::shared_ptr<DialogResult>(dialog_), window_, filters,
                               int(std::size(filters)), location.empty() ? nullptr : location.c_str(), false);
    }

    void PollResults()
    {
        if (graphics_cards_listing_ && graphics_cards_scan_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            graphics_cards_listing_ = false;
            graphics_cards_ = graphics_cards_scan_.get();
            platform::SessionLogLine(platform::DescribeGraphicsCards(graphics_cards_));
        }
        if (updates_)
        {
            update_ = updates_->Snapshot();
            if (update_prompt_ != UpdatePrompt::None && update_.state != UpdateService::State::Checking)
            {
                // After the hidden switch: the menu, with whatever test builds there are.
                if (update_prompt_ == UpdatePrompt::Reveal)
                {
                    if (!update_.test_builds.empty()) test_menu_ = true;
                }
                else if (update_.state == UpdateService::State::Available && update_.offer)
                {
                    // After the start only newer builds: an update or a newer test build.
                    const OfferKind kind = ClassifyOffer(updates_->Build(), *update_.offer);
                    if (update_prompt_ != UpdatePrompt::Newer || kind == OfferKind::Update
                        || (kind == OfferKind::TestBuild && IsNewer(updates_->Build(), *update_.offer)))
                        update_dialog_ = true;
                }
                else if (update_prompt_ == UpdatePrompt::Switch && ChooseTestBuild())
                    test_menu_ = true;
                // Nothing to install or choose: the switch stands.
                if (!update_dialog_ && !test_menu_) switch_from_.reset();
                update_prompt_ = UpdatePrompt::None;
            }
            if (update_.state == UpdateService::State::Available && update_capture_dialog_)
            {
                update_capture_dialog_ = false;
                update_dialog_ = true;
            }
            if (update_.state == UpdateService::State::Available && update_capture_install_)
            {
                if (update_.install_blocker.empty()) updates_->Install();
                else update_capture_install_ = false;
            }
            if (update_.state == UpdateService::State::InstallFailed) update_capture_install_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(dialog_->mutex);
            if (dialog_->ready)
            {
                dialog_->ready = false;
                if (!dialog_->error.empty()) Notice(dialog_->error, NoticeKind::Error);
                else if (!dialog_->path.empty()) SelectDisc(dialog_->path);
            }
        }
        {
            std::lock_guard<std::mutex> lock(folder_dialog_->mutex);
            if (folder_dialog_->ready)
            {
                folder_dialog_->ready = false;
                if (!folder_dialog_->error.empty()) Notice(folder_dialog_->error, NoticeKind::Error);
                else if (!folder_dialog_->path.empty()) FolderChosen(folder_dialog_->path);
            }
        }
        {
            std::lock_guard<std::mutex> lock(gamecube_dialog_->mutex);
            if (gamecube_dialog_->ready)
            {
                gamecube_dialog_->ready = false;
                if (!gamecube_dialog_->error.empty()) Notice(gamecube_dialog_->error, NoticeKind::Error);
                else if (!gamecube_dialog_->path.empty()) GameCubeDiscChosen(PathFromUtf8(gamecube_dialog_->path));
            }
        }
        if (pending_ && future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto result = future_.get();
            pending_ = false;
            if (result.selection == DiscSelection())
            {
                KeepCheck(PathFromUtf8(result.selection), result);
                check_ = std::move(result);
                if (check_.info && !SupportedRelease(*check_.info))
                {
                    region_prompt_info_ = *check_.info;
                    region_prompt_ = true;
                }
            }
            // The disc changed during the check: check the new one.
            else CheckDisc();
        }
        if (candidate_pending_ && candidate_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            candidate_pending_ = false;
            CandidateChecked(candidate_.get());
        }
        if (library_scanning_ && library_scan_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            LibraryScanDone(library_scan_.get());
        if (searching_ && search_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            searching_ = false;
            DiscSearchDone(search_.get());
        }
    }

    // Advanced -> Graphics card (display.gpu): Automatic or one of the PC's cards.
    void GraphicsCardRow()
    {
#if defined(_WIN32)
        Row("Graphics card", "Automatic uses the fastest. A card switched off (in Device Manager or by a laptop's GPU "
                             "mode) is not listed. Used the next time you start the game.");
#else
        Row("Graphics card", "Automatic uses the fastest; cards with a Vulkan driver are listed. Used the next time "
                             "you start the game.");
#endif
        std::optional<GraphicsCardId> chosen;
        try { chosen = ParseGraphicsCard(draft_.gpu); }
        catch (const std::invalid_argument&) {}
        std::string preview = "Automatic (fastest)";
        if (chosen)
        {
            preview = graphics_cards_listing_ ? draft_.gpu : "Not found: " + draft_.gpu;
            for (const auto& card : graphics_cards_)
                if (card.id == *chosen) preview = card.name;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##graphics_card", preview.c_str()))
        {
            if (ImGui::Selectable("Automatic (fastest)", !chosen)) { draft_.gpu = "auto"; dirty_ = true; }
            for (const auto& card : graphics_cards_)
            {
                // Two cards of one name (some laptops list the same model twice) show their IDs.
                bool twin = false;
                for (const auto& other : graphics_cards_) twin = twin || (other.id != card.id && other.name == card.name);
                const std::string label = card.name + (twin ? " (" + GraphicsCardSetting(card.id) + ")" : "")
                    + "##" + GraphicsCardSetting(card.id);
                if (ImGui::Selectable(label.c_str(), chosen && *chosen == card.id))
                { draft_.gpu = GraphicsCardSetting(card.id); dirty_ = true; }
            }
            if (graphics_cards_listing_) ImGui::TextDisabled("Looking for graphics cards...");
            ImGui::EndCombo();
        }
    }

    // Standard gamepads in the order the game numbers them (gamepad1-4);
    // Wii Remotes have their own driver and are left out.
    void RefreshGamepad()
    {
        // Every pad stays open: the gamepad setup takes presses from any of them.
        StopGyroPreview();
        for (auto* pad : pads_) SDL_CloseGamepad(pad);
        pads_.clear();
        gamepad_ = nullptr;
        gamepad_names_.clear();
        trigger_held_.clear();
        stick_held_.clear();
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        for (int n = 0; ids && n < count; ++n)
        {
            const auto vendor = SDL_GetGamepadVendorForID(ids[n]), product = SDL_GetGamepadProductForID(ids[n]);
            if (vendor == 0x057e && (product == 0x0306 || product == 0x0330)) continue;
            const char* name = SDL_GetGamepadNameForID(ids[n]);
            gamepad_names_.push_back(name ? name : "Controller");
            pads_.push_back(SDL_OpenGamepad(ids[n]));
            if (!gamepad_) gamepad_ = pads_.back();
        }
        SDL_free(ids);
    }

    void RefreshDisplays()
    {
        displays_.clear();
        display_ids_.clear();
        int count = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&count);
        const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        for (int i = 0; ids && i < count; ++i)
        {
            const char* name = SDL_GetDisplayName(ids[i]);
            std::string label = std::to_string(i + 1) + "  " + (name && *name ? name : "Display");
            if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]))
                label += "  (" + std::to_string(mode->w) + " x " + std::to_string(mode->h) + ")";
            if (ids[i] == primary) label += "  - primary";
            displays_.push_back(std::move(label));
            display_ids_.push_back(ids[i]);
        }
        SDL_free(ids);
    }

    bool OpenPath(const fs::path& path)
    {
        if (!SDL_OpenURL(FileUrl(path).c_str()))
        {
            Notice(std::string("Cannot open the folder: ") + SDL_GetError(), NoticeKind::Error);
            return false;
        }
        return true;
    }

    bool DiscReady() const
    {
        return config_ok_ && !pending_ && check_.info && check_.selection == DiscSelection();
    }

    // Why Play is unavailable, or empty when the game can start.
    std::string PlayBlocker() const
    {
#ifndef MSCHARGED_HAS_ORIGINAL_FRONTEND
        return "This launcher build does not include the game. Build the release preset to play.";
#else
        // The installed files belong to the next launcher: never start them from this one.
        if (update_.state == UpdateService::State::Installing || update_.state == UpdateService::State::Installed)
            return "Finishing the update...";
        if (!config_ok_) return "Fix the settings file problem shown below first.";
        if (searching_) return "Looking for your game image...";
        if (pending_) return "Checking your game image...";
        if (DiscSelection().empty())
            return autochoose_ != AutoChoice::None && library_scanning_
                ? "Reading your game folder..."
                : "Choose your Mario Strikers Charged image (ISO, RVZ or WBFS) on the Game page.";
        if (!PinnedMissing().empty()) return "Your game image is missing. " + PinnedMissing();
        if (!check_.info || check_.selection != DiscSelection())
            return check_.error.empty() ? "Check your image on the Game page." : check_.error;
        if (!SupportedRelease(*check_.info)) return UnplayableReason(*check_.info);
        const auto settings = EffectiveLaunch().settings;
        try { ResolveGraphicsBackend(settings.backend); }
        catch (const std::invalid_argument& error) { return error.what(); }
        if (settings.aspect != "auto" && settings.aspect != "4:3" && settings.aspect != "16:9")
            return "Choose Automatic, 4:3 or 16:9 as the aspect ratio on the Display page.";
        // A language this release does not have starts as Automatic (the Game page says so).
        return {};
#endif
    }

    void Play(bool& quit)
    {
        if (!PlayBlocker().empty()) return;
        // Unsaved changes: ask whether to start with or without them.
        if (dirty_) { play_prompt_ = true; return; }
        Start(quit);
    }

    void Start(bool& quit)
    {
        frontend_launch_ = EffectiveLaunch();
        quit = true;
    }

    std::string LaunchCommand() const
    {
        const auto launch = EffectiveLaunch();
        const auto& s = launch.settings;
        std::string command = Quote(PathUtf8(base_ / kExecutableName));
        if (!launch.disc_path.empty()) command += " --disc " + Quote(PathUtf8(launch.disc_path));
        command += s.fullscreen ? " --fullscreen" : " --window";
        command += " --size " + std::to_string(s.width) + "x" + std::to_string(s.height);
        command += " --aspect " + s.aspect;
        command += " --frame-rate " + s.frame_rate;
        // Tweaks come from the INI; without any here, keep the INI's off too.
        if (s.GameTweakMask() == 0 && file_.settings.GameTweakMask() != 0) command += " --no-tweaks";
        return command;
    }

    // ------------------------------------------------------------- layout
    void Draw(bool& quit)
    {
        auto& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && config_ok_ && dirty_) Save();
        const ImVec2 size = io.DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        const float sidebar = size.x < Dp(1000) ? Dp(216) : Dp(244);
        DrawSidebar(sidebar, size.y);
        DrawMain({sidebar, 0}, {size.x - sidebar, size.y}, quit);
        DrawClosePrompt(quit);
        DrawPlayPrompt(quit);
        DrawImportPrompt();
        DrawRegionPrompt();
        DrawUpdateDialog();
        // The new files are in place: restart into them (unsaved settings are
        // saved or dropped in the update dialog first).
        if (update_.state == UpdateService::State::Installed && !dirty_ && !restart_)
        {
            restart_ = true;
            quit = true;
        }
        ImGui::End();
    }

    void DrawSidebar(float width, float height)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled({0, 0}, {width, height}, Col(color::sidebar));
        draw->AddLine({width - 1, 0}, {width - 1, height}, Col(color::border, 0.7f), 1.0f);
        const float pad = Dp(24);
        float y = Dp(30);
        DrawTracked(draw, fonts.caption, {pad, y}, Col(color::muted), "NATIVE PC PORT", Dp(2.4f));
        y += FontSize(fonts.caption) + Dp(2);
        // "RECHARGED" with a soft glow and a vertical lime-to-green gradient.
        for (int i = 3; i >= 1; --i)
            DrawLabel(draw, fonts.title, {pad, y + Dp(float(i) * 0.7f)}, Col(color::accent, 0.08f * float(i)), "RECHARGED");
        const int start = draw->VtxBuffer.Size;
        DrawLabel(draw, fonts.title, {pad, y}, IM_COL32_WHITE, "RECHARGED");
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, {pad, y},
                                                     {pad, y + FontSize(fonts.title)}, Col(color::accent_hi), Col(color::accent));
        y += FontSize(fonts.title) + Dp(4);
        DrawLabel(draw, fonts.caption, {pad, y}, Col(color::dim), "Mario Strikers Charged");
        y += FontSize(fonts.caption) + Dp(28);

        ImGui::SetCursorPos({Dp(12), y});
        for (int i = 0; i < PageCount; ++i)
        {
            ImGui::SetCursorPosX(Dp(12));
            if (NavItem(page_names[i], page_icons[i], page_ == i, width - Dp(24))) page_ = i;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + Dp(4));
        }

        // Version at the bottom of the sidebar; test builds show a badge above
        // it that opens their pull request.
        const float badge = height - Dp(40), nav_end = ImGui::GetCursorPosY();
        if (nav_end < badge - Dp(36) && std::string_view(build::channel) == "test")
        {
            ImGui::SetCursorPos({pad - Dp(2), badge - Dp(36)});
            if (TestBuildBadge("TEST BUILD  -  PR #" + std::to_string(build::pull_request)))
                SDL_OpenURL(BuildNotesUrl(CurrentBuild()).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("This is a test build of pull request #%d.\nClick to open the pull request.",
                                  build::pull_request);
        }
        if (nav_end < badge)
        {
            ImGui::SetCursorPos({pad, badge});
            ImGui::PushFont(fonts.caption);
            ImGui::PushStyleColor(ImGuiCol_Text, color::dim);
            ImGui::TextUnformatted(build::version);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }

    bool TestBuildBadge(const std::string& label)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float tracking = Dp(0.8f);
        const ImVec2 size{Dp(38) + TrackedWidth(fonts.caption, label.c_str(), tracking), Dp(26)};
        const bool pressed = ImGui::InvisibleButton("##test_build", size);
        const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 12.0f);
        const ImVec2 max{pos.x + size.x, pos.y + size.y};
        draw->AddRectFilled(pos, max, Col(color::warning, 0.10f + 0.08f * hover), size.y * 0.5f);
        draw->AddRect(pos, max, Col(color::warning, 0.35f + 0.30f * hover), size.y * 0.5f, 0, Dp(1));
        DrawIcon(draw, Icon::Flask, {pos.x + Dp(16), pos.y + size.y * 0.5f}, Dp(13), Col(color::warning));
        DrawTracked(draw, fonts.caption, {pos.x + Dp(29), pos.y + (size.y - FontSize(fonts.caption)) * 0.5f},
                    Col(color::warning), label.c_str(), tracking);
        return pressed;
    }

    void DrawMain(ImVec2 origin, ImVec2 size, bool& quit)
    {
        if (page_ != PageControls && keyboard_view_) CloseKeyboardView();
        if (page_ != PageControls && gamepad_view_) CloseGamepadView();
        if (page_ != PageGameplay) texture_page_open_ = false;
        // Bluetooth inquiries can disturb other Bluetooth devices: search only
        // while the Controls page is open (never in development captures).
        bluetooth_search_.SetActive(page_ == PageControls && config_ok_ && draft_.bluetooth_search && !capture_);
        // Which Wii Remotes answer: for the Players card, and for the Play
        // page when a player uses a remote.
        if (page_ == PageControls || (page_ == PagePlay && RemotePlayer())) PollWiimotes();
        const float bar = Dp(74);
        const ImVec2 content{size.x, size.y - bar};
        ImGui::SetCursorPos(origin);
        if (page_ == PagePlay) DrawPlayPage(origin, content, quit);
        else
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(36, 30));
            ImGui::BeginChild("##page", content, ImGuiChildFlags_AlwaysUseWindowPadding);
            const float available = ImGui::GetContentRegionAvail().x;
            const float column = std::min(available, Dp(900));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - column) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            ImGui::BeginChild("##column", {column, 0}, ImGuiChildFlags_AutoResizeY);
            ImGui::BeginDisabled(!config_ok_ && page_ != PageAbout);
            switch (page_)
            {
            case PageGame: GamePage(); break;
            case PageDisplay: DisplayPage(); break;
            case PageAudio: AudioPage(); break;
            case PageControls: ControlsPage(); break;
            case PageGameplay: GameplayPage(); break;
            case PageAdvanced: AdvancedPage(); break;
            default: AboutPage(); break;
            }
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        DrawBottomBar({origin.x, origin.y + content.y}, {size.x, bar}, quit);
    }

    void DrawPlayPage(ImVec2 origin, ImVec2 size, bool& quit)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const float hero = std::clamp(size.y * 0.58f, Dp(250), Dp(440));
        hero_ = {origin.x, origin.y, size.x, hero};
        hero_visible_ = true;
        const ImVec2 hero_max{origin.x + size.x, origin.y + hero};
        // Readability gradients over the artwork.
        draw->AddRectFilledMultiColor(origin, {origin.x + size.x * 0.75f, hero_max.y}, Col(color::window, 0.94f),
                                      Col(color::window, 0.0f), Col(color::window, 0.0f), Col(color::window, 0.94f));
        draw->AddRectFilledMultiColor({origin.x, origin.y + hero * 0.45f}, hero_max, Col(color::window, 0.0f),
                                      Col(color::window, 0.0f), Col(color::window, 1.0f), Col(color::window, 1.0f));
        draw->AddRectFilledMultiColor(origin, {hero_max.x, origin.y + Dp(60)}, Col(color::window, 0.45f),
                                      Col(color::window, 0.45f), Col(color::window, 0.0f), Col(color::window, 0.0f));

        const float pad = Dp(44);
        float y = origin.y + std::max(Dp(34), hero * 0.20f);
        DrawTracked(draw, fonts.caption, {origin.x + pad, y}, Col(color::accent, 0.95f), "NATIVE PC PORT", Dp(2.6f));
        y += FontSize(fonts.caption) + Dp(6);
        // The title: "RECHARGED" with a soft glow and a vertical lime-to-green gradient.
        for (int i = 4; i >= 1; --i)
            DrawLabel(draw, fonts.hero, {origin.x + pad, y + Dp(float(i))}, Col(color::accent, 0.07f * float(i)), "RECHARGED");
        const int start = draw->VtxBuffer.Size;
        DrawLabel(draw, fonts.hero, {origin.x + pad, y}, IM_COL32_WHITE, "RECHARGED");
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, {origin.x, y},
                                                     {origin.x, y + FontSize(fonts.hero)}, Col(color::accent_hi),
                                                     Col(color::accent));
        y += FontSize(fonts.hero) + Dp(4);
        FillGradient(draw, {origin.x + pad + Dp(2), y}, {origin.x + pad + Dp(66), y + Dp(3)}, Col(color::accent_hi),
                     Col(color::accent, 0.0f), Dp(1.5f), true);
        y += Dp(13);
        // The subtitle: the game in white, the rest softer; two lines in a narrow window.
        const char* game = "Mario Strikers Charged";
        const char* rest = ", rebuilt to run natively on your PC.";
        const float game_width = TextSize(fonts.heading, game).x;
        const bool one_line = game_width + TextSize(fonts.heading, rest).x <= size.x * 0.75f - pad;
        DrawLabelShadowed(draw, fonts.heading, {origin.x + pad, y}, IM_COL32_WHITE, game, 1.2f);
        if (one_line)
            DrawLabelShadowed(draw, fonts.heading, {origin.x + pad + game_width, y}, Col(color::text, 0.86f), rest, 1.2f);
        else
        {
            DrawLabelShadowed(draw, fonts.heading, {origin.x + pad + game_width, y}, Col(color::text, 0.86f), ",", 1.2f);
            DrawLabelShadowed(draw, fonts.heading, {origin.x + pad, y + FontSize(fonts.heading) + Dp(2)},
                              Col(color::text, 0.86f), rest + 2, 1.2f);
        }
        DrawUpdateBar(origin, size.x, pad);

        // Play button and status, overlapping the bottom of the artwork.
        const std::string blocker = PlayBlocker();
        const float button_y = hero_max.y - Dp(30);
        ImGui::SetCursorScreenPos({origin.x + pad, button_y});
        const bool menu_open = ImGui::IsPopupOpen("play_options_menu");
        const auto play = SplitPrimaryButton("play", "PLAY", Dp(230, 62),
                                             {Dp(58), Icon::Gear, menu_open, draft_.performance_mode},
                                             blocker.empty(), Icon::Play, true);
        if (play.button) Play(quit);
        if (!blocker.empty() && play.button_hovered) ImGui::SetTooltip("%s", blocker.c_str());
        if (play.segment_hovered && !menu_open) ImGui::SetTooltip("Play options");
        if (play.segment) ImGui::OpenPopup("play_options_menu");
        DrawPlayOptionsMenu({origin.x + pad + Dp(230), button_y});
        DrawPlayStatus(blocker, {origin.x + pad + Dp(230 + 58 + 24), button_y + Dp(31)});

        // Summary cards.
        const float cards_y = button_y + Dp(62) + Dp(30);
        const float footer = Dp(30);
        if (cards_y + Dp(118) + footer + Dp(12) <= origin.y + size.y)
        {
            const float gap = Dp(16);
            const float width = (size.x - pad * 2 - gap * 2) / 3.0f;
            ImGui::SetCursorScreenPos({origin.x + pad, cards_y});
            SummaryCard("##disc_card", "Game image", Icon::Disc, width, PageGame, DiscSummary());
            ImGui::SameLine(0, gap);
            SummaryCard("##display_card", "Display", Icon::Display, width, PageDisplay, DisplaySummary());
            ImGui::SameLine(0, gap);
            SummaryCard("##language_card", "Language & audio", Icon::Audio, width, PageAudio, LanguageSummary());
        }
    }

    // The gear's menu opens above it, like a launcher's game options.
    void DrawPlayOptionsMenu(ImVec2 anchor)
    {
        const float width = Dp(300);
        ImGui::SetNextWindowPos({anchor.x, anchor.y - Dp(8)}, ImGuiCond_Always, {0.0f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, Dp(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(10));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(color::card, color::window, 0.25f));
        if (ImGui::BeginPopup("play_options_menu"))
        {
            if (MenuToggle("performance_mode", "Performance mode",
                           "No crowd, ambient effects, bloom or antialiasing. Gameplay is unchanged.",
                           &draft_.performance_mode, width))
                dirty_ = true;
            MenuSeparator(width);
            if (MenuAction("display_settings", "Display settings", width))
            {
                page_ = PageDisplay;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    // Something to know before playing, under the Play status: a small glass
    // pill that opens the page where it is changed.
    struct StatusNote
    {
        Icon icon;
        ImVec4 tint;
        bool alert;            // the text takes the tint too
        std::string text;      // what is so
        std::string secondary; // what follows from it; left out when the pill would be too wide
        std::string tooltip;
        int page;
    };

    // At most `max_width` wide; true when clicked.
    bool StatusPill(const char* id, const StatusNote& note, float max_width)
    {
        const auto& fonts = CurrentFonts();
        const float height = Dp(28), left = Dp(30), right = Dp(26);
        const float room = std::max(Dp(40), max_width - left - right);
        const std::string secondary = note.secondary.empty() ? std::string() : "  \xC2\xB7  " + note.secondary;
        const bool both = !secondary.empty() && TextSize(fonts.caption, (note.text + secondary).c_str()).x <= room;
        const std::string text = Elide(fonts.caption, note.text, room);
        const ImVec2 label = TextSize(fonts.caption, text.c_str());
        const float extra = both ? TextSize(fonts.caption, secondary.c_str()).x : 0.0f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size{left + label.x + extra + right, height};
        const bool pressed = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        const float hover = Animate(ImGui::GetItemID(), hovered, 12.0f);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 max{pos.x + size.x, pos.y + size.y};
        const float rounding = height * 0.5f, middle = pos.y + height * 0.5f;
        // Glass over the artwork, as the update status, washed with the note's colour.
        draw->AddRectFilled(pos, max,
                            Col(Mix(Mix(color::window, color::card_hover, 0.45f + 0.55f * hover), note.tint, 0.07f), 0.9f),
                            rounding);
        draw->AddRect(pos, max, Col(Mix(color::border, note.tint, 0.30f + 0.35f * hover), 0.95f), rounding, 0, Dp(1));
        DrawIcon(draw, note.icon, {pos.x + Dp(16), middle}, Dp(14), Col(note.tint));
        const ImVec4 ink = note.alert ? Mix(color::text, note.tint, 0.55f - 0.25f * hover)
                                      : Mix(color::muted, color::text, 0.7f + 0.3f * hover);
        DrawLabel(draw, fonts.caption, {pos.x + left, middle - label.y * 0.5f}, Col(ink), text.c_str());
        if (both)
            DrawLabel(draw, fonts.caption, {pos.x + left + label.x, middle - label.y * 0.5f}, Col(color::muted),
                      secondary.c_str());
        DrawChevron(draw, {max.x - Dp(14) + Dp(2) * hover, middle}, Dp(10), Col(Mix(color::dim, color::text, hover)));
        if (hovered) ImGui::SetTooltip("%s", note.tooltip.c_str());
        return pressed;
    }

    // Right of the Play button, centred on it (`anchor`: the left edge at the
    // button's middle): whether the game can start and, under that, what to
    // know first, each opening its page: a controller that is not connected
    // and who plays instead, the gameplay tweaks that are on.
    void DrawPlayStatus(const std::string& blocker, ImVec2 anchor)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        ImVec4 tint = color::accent;
        Icon icon = Icon::Check;
        bool spinner = false;
        std::string headline = "Ready to play";
        std::string detail;
        const bool missing = !PinnedMissing().empty() && !pending_ && !searching_;
        if (pending_ || searching_)
        {
            tint = color::info;
            icon = Icon::None;
            spinner = true;
            headline = searching_ ? "Looking for your game image" : "Checking your game image";
            detail = searching_ ? "In the game folder and the usual places..." : "Reading the game partition...";
        }
        else if (missing)
        {
            // Never another image instead: the player decides.
            tint = color::warning;
            icon = Icon::Warning;
            const auto info = PinnedInfo();
            headline = "Your game image is missing";
            const std::string name = PathUtf8(PathFromUtf8(DiscSelection()).filename());
            detail = (info ? RegionRevision(*info) + "  \xC2\xB7  " : std::string())
                + (check_.folder_missing ? "The drive or folder with " + name + " is not available."
                                         : name + " is no longer in its folder.");
        }
        else if (!blocker.empty())
        {
            tint = DiscSelection().empty() ? color::info : color::warning;
            icon = DiscSelection().empty() ? Icon::Disc : Icon::Warning;
            headline = DiscSelection().empty() ? "Choose your game image" : "Not ready yet";
            detail = blocker;
        }
        else if (check_.info)
            detail = ReleaseTitle(*check_.info) + "  \xC2\xB7  " + RegionRevision(*check_.info) + "  \xC2\xB7  "
                + check_.info->game_id;
        std::vector<StatusNote> notes;
        if (blocker.empty())
        {
            // A player's controller that is not connected, and who plays instead.
            const auto lineup = StartLineup();
            const auto brief = MissingControllersBrief(draft_.players, lineup);
            if (!brief.missing.empty())
            {
                const bool several = std::count(lineup.missing.begin(), lineup.missing.end(), true) > 1;
                notes.push_back({Icon::Gamepad, color::warning, true, brief.missing, brief.instead,
                                 MissingControllersNote(draft_.players, lineup) + "\nConnect "
                                     + (several ? "them" : "it") + " or choose who plays on the Controls page.",
                                 PageControls});
            }
            // Gameplay tweaks change the original game: never silently.
            std::string names;
            int count = 0;
            for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
                if (draft_.tweaks[n])
                {
                    names += std::string("\n\xE2\x80\xA2 ") + kGameTweaks[n].name;
                    ++count;
                }
            if (count)
                notes.push_back({Icon::Target, color::accent, false,
                                 std::to_string(count) + (count == 1 ? " gameplay tweak on" : " gameplay tweaks on"), "",
                                 "Gameplay tweaks on:" + names, PageGameplay});
        }
        // The notes take the disc line's place (the Game image card has it).
        if (!notes.empty()) detail.clear();

        const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - Dp(28);
        const float text_x = anchor.x + Dp(30);
        const float wrap = std::max(Dp(120), limit - text_x);
        const float caption = FontSize(fonts.caption);
        const float detail_height =
            detail.empty() ? 0.0f : fonts.caption->CalcTextSizeA(caption, FLT_MAX, wrap, detail.c_str()).y;
        const float pill = Dp(28), pill_gap = Dp(6);
        float height = Dp(26) + detail_height;
        if (!notes.empty()) height += Dp(4) + float(notes.size()) * (pill + pill_gap) - pill_gap;
        if (missing) height += Dp(4) + Dp(32);
        const ImVec2 pos{anchor.x, std::floor(anchor.y - height * 0.5f)};

        if (spinner) Spinner({pos.x + Dp(11), pos.y + Dp(12)}, Dp(8), Col(tint));
        else if (icon != Icon::None) DrawIcon(draw, icon, {pos.x + Dp(11), pos.y + Dp(12)}, Dp(20), Col(tint));
        DrawLabel(draw, fonts.label, {text_x, pos.y + Dp(1)}, Col(color::text), headline.c_str());
        float y = pos.y + Dp(26);
        if (!detail.empty())
        {
            draw->AddText(fonts.caption, caption, {text_x, y}, Col(color::muted), detail.c_str(), nullptr, wrap);
            y += detail_height;
        }
        ImGui::PushID("play_status");
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            ImGui::SetCursorScreenPos({pos.x + Dp(22), y + Dp(4) + float(i) * (pill + pill_gap)});
            ImGui::PushID(int(i));
            if (StatusPill("##note", notes[i], limit - pos.x - Dp(22))) page_ = notes[i].page;
            ImGui::PopID();
        }
        ImGui::PopID();
        if (missing)
        {
            ImGui::SetCursorScreenPos({pos.x + Dp(22), y + Dp(4)});
            MissingImageActions(limit);
        }
    }

    // What to do about a game image that has gone: locate it, use a copy of
    // the same release from the game folder, or choose another image. The
    // buttons wrap within `right` (a screen x).
    void MissingImageActions(float right)
    {
        ImGui::PushID("missing_image");
        const float height = Dp(32), gap = Dp(6);
        const float left = ImGui::GetCursorScreenPos().x;
        bool first = true;
        const auto place = [&](const char* label, Icon icon) {
            const float width = GhostWidth(label, icon);
            if (!first)
            {
                ImGui::SameLine(0, gap);
                if (ImGui::GetCursorScreenPos().x + width > right)
                    ImGui::SetCursorScreenPos({left, ImGui::GetItemRectMax().y + Dp(2)});
            }
            first = false;
            return ImVec2{width, height};
        };
        if (GhostButton("locate", "Locate...", place("Locate...", Icon::Folder), !DialogActive(), Icon::Folder)) Locate();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose the image file where it is now.");
        const auto info = PinnedInfo();
        const auto copies = info ? SameRelease(library_, info->game_id, info->revision, PathFromUtf8(DiscSelection()))
                                 : std::vector<const LibraryImage*>{};
        if (!copies.empty())
        {
            const auto* copy = copies.front();
            if (GhostButton("copy", "Use the copy", place("Use the copy", Icon::Check), true, Icon::Check))
                UseImage(copy->path, copy->probe.info, copy->size);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Your game folder has the same release (%s):\n%s",
                                  RegionRevision(copy->probe.info).c_str(), PathUtf8(copy->path).c_str());
        }
        if (GhostButton("another", "Choose another", place("Choose another", Icon::Disc), true, Icon::Disc))
            page_ = PageGame;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose another image on the Game page.");
        ImGui::PopID();
    }

    bool DialogActive() const
    {
        std::lock_guard<std::mutex> lock(dialog_->mutex);
        return dialog_->active;
    }

    // Browse for the pinned image where it is now, starting in its old folder
    // (or the nearest folder above it that is still there).
    void Locate() { Browse(ExistingFolder(PathFromUtf8(DiscSelection()).parent_path())); }

    struct Summary { std::string value; std::string detail; ImVec4 tint; };

    // The pinned image: its region and revision first.
    Summary DiscSummary() const
    {
        if (searching_) return {"Looking...", "Searching for your game image", color::info};
        if (pending_) return {"Checking...", "Reading the image's header", color::info};
        if (DiscSelection().empty()) return {"No image chosen", "Choose one on the Game page", color::warning};
        const std::string name = PathUtf8(PathFromUtf8(DiscSelection()).filename());
        if (!PinnedMissing().empty())
        {
            const auto info = PinnedInfo();
            return {info ? RegionRevision(*info) + " - missing" : "Image missing", name + " cannot be found",
                    color::warning};
        }
        if (check_.info && check_.selection == DiscSelection())
        {
            const auto& info = *check_.info;
            return {RegionRevision(info), info.game_id + "  \xC2\xB7  " + info.format + "  \xC2\xB7  " + name,
                    SupportedRelease(info) ? color::accent : color::warning};
        }
        if (!check_.error.empty()) return {"Image problem", check_.error, color::danger};
        return {name, "Not checked yet", color::muted};
    }

    Summary DisplaySummary() const
    {
        const auto s = EffectiveLaunch().settings;
        std::string aspect = s.aspect == "auto" ? "Automatic aspect" : s.aspect == "stretch" ? "Stretched" : s.aspect;
        std::string detail = aspect + (s.vsync ? "  -  VSync on" : "  -  VSync off");
        if (s.frame_rate != "original") detail += "  -  " + FrameRateLabel(s.frame_rate);
        if (s.performance_mode) detail += "  -  Performance mode";
        if (!s.show_hud)
        {
            const auto hud = platform::HotkeyLabel(platform::ParseHotkeys(s.hotkeys)[HotkeyHud]);
            detail += "  -  HUD hidden" + (hud.empty() ? std::string() : " (" + hud + ")");
        }
        return {s.fullscreen ? "Fullscreen" : std::to_string(s.width) + " x " + std::to_string(s.height) + " window",
                detail, color::info};
    }

    Summary LanguageSummary() const
    {
        const auto s = EffectiveLaunch().settings;
        const auto choice = ChooseLanguage(Release(), s.language, pc_languages_);
        const std::string language = choice.automatic ? "Automatic (" + std::string(choice.language->label) + ")"
                                                      : std::string(choice.language->label);
        std::string audio = s.mute ? "Audio muted" : "Volume " + std::to_string(s.master_volume) + "%";
        if (!s.mute && s.sound_volumes != DefaultSoundVolumes()) audio += "  -  Custom sound levels";
        return {language + " text", audio, color::info};
    }

    void SummaryCard(const char* id, const char* title, Icon icon, float width, int page, const Summary& summary)
    {
        const auto& fonts = CurrentFonts();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Mix(color::card, color::window, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_Border, color::border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Dp(16));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(18, 16));
        ImGui::BeginChild(id, {width, Dp(118)}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(pos, {pos.x + Dp(30), pos.y + Dp(30)}, Col(summary.tint, 0.14f), Dp(9));
        DrawIcon(draw, icon, {pos.x + Dp(15), pos.y + Dp(15)}, Dp(16), Col(summary.tint));
        DrawLabel(draw, fonts.caption, {pos.x + Dp(40), pos.y + Dp(6)}, Col(color::muted), title);
        const float inner = ImGui::GetContentRegionAvail().x;
        DrawChevron(draw, {pos.x + inner - Dp(4), pos.y + Dp(14)}, Dp(14), Col(color::dim));
        ImGui::SetCursorScreenPos({pos.x, pos.y + Dp(42)});
        ImGui::PushFont(fonts.label);
        ImGui::PushStyleColor(ImGuiCol_Text, color::text);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextUnformatted(summary.value.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
        ImGui::PushFont(fonts.caption);
        ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextUnformatted(summary.detail.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        // The whole card opens its settings page.
        ImGui::SetCursorScreenPos(ImGui::GetWindowPos());
        if (ImGui::InvisibleButton("##open", ImGui::GetWindowSize())) page_ = page;
        const float hover = Animate(ImGui::GetID("##hover"), ImGui::IsItemHovered(), 12.0f);
        if (hover > 0.0f)
        {
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 max{min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y};
            ImGui::GetWindowDrawList()->AddRect(min, max, Col(color::accent, 0.55f * hover), Dp(16), 0, Dp(1.5f));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    void DrawBottomBar(ImVec2 origin, ImVec2 size, bool& quit)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, Col(Mix(color::window, color::sidebar, 0.7f)));
        draw->AddLine(origin, {origin.x + size.x, origin.y}, Col(color::border, 0.8f), 1.0f);
        const float pad = Dp(28);
        const float button_h = Dp(42);
        const float button_y = origin.y + (size.y - button_h) * 0.5f;

        // Right-aligned actions: Revert, Save, Play.
        float x = origin.x + size.x - pad;
        if (page_ != PagePlay)
        {
            x -= Dp(140);
            ImGui::SetCursorScreenPos({x, button_y});
            const std::string blocker = PlayBlocker();
            if (PrimaryButton("bar_play", "Play", {Dp(140), button_h}, blocker.empty(), Icon::Play)) Play(quit);
            if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", blocker.c_str());
            x -= Dp(12);
        }
        x -= Dp(118);
        ImGui::SetCursorScreenPos({x, button_y});
        if (SecondaryButton("save", dirty_ ? "Save" : "Saved", {Dp(118), button_h}, config_ok_ && (dirty_ || !file_.exists),
                            dirty_ ? Icon::Check : Icon::None))
            Save();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Saves to %s (Ctrl+S)", PathUtf8(file_.path).c_str());
        x -= Dp(10) + Dp(104);
        ImGui::SetCursorScreenPos({x, button_y});
        if (SecondaryButton("revert", "Revert", {Dp(104), button_h}, dirty_ || !config_ok_, Icon::Refresh)) Reload();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Reload the settings file and discard unsaved changes.");
        float right = x - Dp(16);
        if (dirty_)
        {
            const float chip = TextSize(fonts.caption, "Unsaved changes").x + Dp(20);
            right -= chip;
            ImGui::SetCursorScreenPos({right, origin.y + (size.y - Dp(26)) * 0.5f});
            Chip("Unsaved changes", color::warning);
            right -= Dp(14);
        }

        // Status message on the left.
        ImVec4 tint = color::muted;
        Icon icon = Icon::Info;
        switch (notice_kind_)
        {
        case NoticeKind::Success: tint = color::accent; icon = Icon::Check; break;
        case NoticeKind::Warning: tint = color::warning; icon = Icon::Warning; break;
        case NoticeKind::Error: tint = color::danger; icon = Icon::Error; break;
        default: break;
        }
        const float text_x = origin.x + pad + Dp(26);
        const float center = origin.y + size.y * 0.5f;
        DrawIcon(draw, icon, {origin.x + pad + Dp(9), center}, Dp(17), Col(tint));
        const float limit = std::max(Dp(60), right - text_x);
        const ImVec2 text = TextSize(fonts.body, notice_.c_str());
        draw->PushClipRect({text_x, origin.y}, {text_x + limit, origin.y + size.y}, true);
        DrawLabel(draw, fonts.body, {text_x, center - text.y * 0.5f},
                  Col(notice_kind_ == NoticeKind::Info ? color::muted : Mix(tint, color::text, 0.35f)), notice_.c_str());
        draw->PopClipRect();
        ImGui::SetCursorScreenPos({text_x, center - text.y * 0.5f});
        ImGui::InvisibleButton("##notice", {std::min(text.x, limit), text.y});
        if (text.x > limit && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", notice_.c_str());
    }

    // Shown once after a check finds a readable but unsupported image: the
    // pinned one, or one chosen to be pinned (which then is not).
    void DrawRegionPrompt()
    {
        if (region_prompt_)
        {
            ImGui::OpenPopup("Unsupported version");
            region_prompt_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({Dp(520), 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Unsupported version", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_AlwaysAutoResize))
        {
            TextColored(fonts.heading, color::text, "This version cannot be played yet");
            ImGui::Dummy({0, Dp(4)});
            TextWrappedColored(fonts.body, color::muted, UnplayableReason(region_prompt_info_).c_str());
            ImGui::Dummy({0, Dp(4)});
            const std::string runs = "This build runs " + PlayableReleasesText()
                + ". The other versions follow as Recharged supports them.";
            TextWrappedColored(fonts.body, color::text, runs.c_str());
            ImGui::Dummy({0, Dp(10)});
            if (PrimaryButton("region_ok", "OK", Dp(120, 42), true) || CancelPressed()
                || ImGui::IsKeyPressed(ImGuiKey_Enter))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void DrawPlayPrompt(bool& quit)
    {
        if (play_prompt_)
        {
            ImGui::OpenPopup("Start with changes?");
            play_prompt_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Start with changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text, "Save your changes before playing?");
            TextColored(fonts.body, color::muted, "Your settings have unsaved changes.");
            ImGui::Dummy({0, Dp(8)});
            if (PrimaryButton("save_play", "Save and play", Dp(170, 42), true, Icon::Play))
            {
                ImGui::CloseCurrentPopup();
                if (Save() && PlayBlocker().empty()) Start(quit);
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("discard_play", "Discard and play", Dp(170, 42)))
            {
                ImGui::CloseCurrentPopup();
                // Back to the saved settings; start only if they can still play.
                Reload();
                if (config_ok_ && PlayBlocker().empty()) Start(quit);
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("cancel_play", "Cancel", Dp(100, 42)) || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void DrawClosePrompt(bool& quit)
    {
        if (close_requested_)
        {
            if (!dirty_) quit = true;
            else ImGui::OpenPopup("Save changes?");
            close_requested_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text, "Save your changes?");
            TextColored(fonts.body, color::muted, "Your settings have unsaved changes.");
            ImGui::Dummy({0, Dp(8)});
            if (PrimaryButton("save_close", "Save and close", Dp(170, 42), true)) { if (Save()) quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("discard", "Discard", Dp(110, 42))) { quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("cancel", "Cancel", Dp(100, 42)) || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // -------------------------------------------------------------- pages
    void GamePage()
    {
        PageHeader("Game", "Your Mario Strikers Charged images, the game's language, the Discord status and your saves.");
        if (options_.launch.disc || options_.launch.size || options_.launch.fullscreen || options_.launch.aspect
            || options_.launch.frame_rate)
        {
            Chip("Command-line options apply to this run only", color::info, Icon::Info);
            ImGui::Dummy({0, Dp(2)});
        }

        GameImagesCard();

        BeginCard("##language", "Game language", "Menus and on-screen text follow the Wii system language.", Icon::Info);
        if (BeginRows("##language_rows"))
        {
            Row("Text language", "Automatic uses your computer's language when this version has it, otherwise "
                                 "English. Saved with your settings.");
            LanguageControl();
            EndRows();
        }
        EndCard();

        DiscordCard();
        SaveDataCard();
    }

    // The Discord status (issue #71): on unless switched off; builds without
    // a Discord application (forks, local builds) cannot show one.
    void DiscordCard()
    {
        BeginCard("##discord", "Discord", "Show what you play on your Discord profile.", Icon::Link);
        if (BeginRows("##discord_rows"))
        {
            const bool available = *mscharged::build::discord_application_id != '\0'
                || !draft_.discord_application_id.empty();
            Row("Discord status", available
                ? "Your profile shows the menu or match you are in, with the score and match time, while the "
                  "Discord app runs on this computer."
                : "This build has no Discord application, so it cannot show a Discord status.");
            ImGui::BeginDisabled(!available);
            if (Toggle("##discord_status", &draft_.discord_status)) dirty_ = true;
            ImGui::EndDisabled();
            EndRows();
        }
        EndCard();
    }

    // Where the game keeps its Wii storage, and importing a save from Dolphin.
    void SaveDataCard()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##data", "Save data", "Your progress and trophies, in the game's own Wii save files.", Icon::Folder);
        bool browsing;
        { std::lock_guard<std::mutex> lock(folder_dialog_->mutex); browsing = folder_dialog_->active; }
        const float buttons = Dp(118) + Dp(10) + Dp(118);
        const std::string default_folder = PathUtf8(DefaultSaveFolder(base_));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - Dp(10));
        if (ImGui::InputTextWithHint("##save_folder", ("Default: " + default_folder).c_str(), &draft_.save_folder))
            dirty_ = true;
        ImGui::SameLine(0, Dp(10));
        if (SecondaryButton("browse_saves", "Browse...", {Dp(118), ImGui::GetFrameHeight()}, !browsing && config_ok_,
                            Icon::Folder))
            BrowseFolder(FolderPurpose::SaveFolder);
        ImGui::SameLine(0, Dp(10));
        if (SecondaryButton("default_saves", "Default", {Dp(118), ImGui::GetFrameHeight()},
                            config_ok_ && !draft_.save_folder.empty(), Icon::Refresh))
        {
            draft_.save_folder.clear();
            dirty_ = true;
        }
        if (draft_.save_folder != file_.settings.save_folder)
        {
            ImGui::Dummy({0, Dp(2)});
            TextWrappedColored(fonts.caption, color::warning,
                               "The game starts with the saves in the new folder (none if it is new). "
                               "Saves in the old folder stay there.");
        }

        const fs::path folder = SaveFolder();
        if (BeginRows("##data_rows"))
        {
            const std::string path = PathUtf8(folder);
            Row("Folder in use", path.c_str());
            if (SecondaryButton("open_data", "Open folder", {ImGui::GetContentRegionAvail().x, Dp(40)},
                                fs::exists(folder), Icon::Folder))
                OpenPath(folder);
            if (!fs::exists(folder) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Created when you first play.");

            const auto found = mscharged::platform::FindDolphinSaves(Release().title_id);
            const std::string description = found.empty()
                ? "Copies a save from the Dolphin emulator. Choose Dolphin's user folder or the game's save folder in it."
                : "Copies the save Dolphin keeps in " + PathUtf8(found.front()) + ".";
            Row("Import from Dolphin", description.c_str());
            const float width = ImGui::GetContentRegionAvail().x;
            if (!found.empty())
            {
                if (SecondaryButton("import_dolphin", "Import Dolphin save", {width, Dp(40)}, config_ok_, Icon::Copy))
                    AskImport(found.front());
                ImGui::Dummy({0, Dp(2)});
            }
            if (SecondaryButton("import_choose", found.empty() ? "Choose folder..." : "Choose another folder...",
                                {width, Dp(40)}, !browsing && config_ok_, Icon::Folder))
                BrowseFolder(FolderPurpose::DolphinSave);
            EndRows();
        }
        EndCard();
    }

    // The checked disc's release, or the USA one before a disc is known.
    const GameRelease& Release() const
    {
        const auto info = PinnedInfo();
        const auto* release = info ? FindRelease(*info) : nullptr;
        return release ? *release : DefaultRelease();
    }

    // What the pinned image is: its check, or what it was when it was last
    // seen (an image that has gone missing).
    std::optional<DiscInfo> PinnedInfo() const
    {
        const auto selection = DiscSelection();
        if (selection.empty()) return std::nullopt;
        if (check_.selection == selection && check_.info) return check_.info;
        const auto* entry = image_cache_.Last(PathFromUtf8(selection));
        if (entry && entry->probe.kind == DiscProbe::Kind::Charged) return entry->probe.info;
        return std::nullopt;
    }

    // The pinned image cannot be found (empty while it exists or is unchecked).
    std::string PinnedMissing() const
    {
        return check_.selection == DiscSelection() && !DiscSelection().empty() ? check_.missing : std::string();
    }

    // The Wii storage of the release's title in the save folder (the USA
    // title uses the folder itself, others a subfolder such as R4QP).
    fs::path SaveFolder() const
    {
        return TitleSaveFolder(ResolveSaveFolder(draft_, file_.path, base_), Release().title_id);
    }

    // `start`: the folder the dialog opens in (or the nearest one above it
    // that exists); the system's choice when empty.
    void BrowseFolder(FolderPurpose purpose, const fs::path& start = {})
    {
        {
            std::lock_guard<std::mutex> lock(folder_dialog_->mutex);
            folder_dialog_->active = true;
        }
        folder_purpose_ = purpose;
        const std::string location = PathUtf8(ExistingFolder(start));
        SDL_ShowOpenFolderDialog(DiscChosen, new std::shared_ptr<DialogResult>(folder_dialog_), window_,
                                 location.empty() ? nullptr : location.c_str(), false);
    }

    // The folder itself, or the nearest folder above it that exists; empty
    // when none does.
    static fs::path ExistingFolder(fs::path folder)
    {
        std::error_code error;
        while (!folder.empty() && !fs::is_directory(folder, error))
        {
            if (folder == folder.parent_path()) return {};
            folder = folder.parent_path();
        }
        return folder;
    }

    void FolderChosen(const std::string& chosen)
    {
        if (folder_purpose_ == FolderPurpose::SaveFolder)
        {
            draft_.save_folder = chosen;
            dirty_ = true;
            return;
        }
        if (folder_purpose_ == FolderPurpose::GameFolder)
        {
            SelectGameFolder(PathFromUtf8(chosen));
            return;
        }
        if (folder_purpose_ == FolderPurpose::TextureFolder)
        {
            draft_.texture_folder = SettingsPath(PathFromUtf8(chosen));
            dirty_ = true;
            return;
        }
        const auto save = mscharged::platform::ResolveDolphinSave(PathFromUtf8(chosen), Release().title_id,
                                                                    kSaveFile);
        if (save.empty())
            Notice("No Mario Strikers Charged save found in " + chosen + ".", NoticeKind::Warning);
        else AskImport(save);
    }

    void AskImport(const fs::path& source)
    {
        import_source_ = source;
        import_prompt_ = true;
    }

    void ImportDolphinSave()
    {
        try
        {
            const auto folder = SaveFolder();
            const auto result = mscharged::platform::ImportDolphinSave(import_source_, folder, SaveTitleOf(Release()),
                                                                       kSaveFile, folder / "backups");
            std::string files;
            for (const auto& name : result.files) files += (files.empty() ? "" : ", ") + name;
            Notice("Dolphin save imported (" + files + ")."
                       + (result.backup.empty() ? "" : " Your previous save is in " + PathUtf8(result.backup) + "."),
                   NoticeKind::Success);
        }
        catch (const std::exception& e) { Notice(std::string("Import failed: ") + e.what(), NoticeKind::Error); }
    }

    void DrawImportPrompt()
    {
        if (import_prompt_)
        {
            ImGui::OpenPopup("Import Dolphin save?");
            import_prompt_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(460), 0}, {Dp(640), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Import Dolphin save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text, "Import the Dolphin save?");
            TextWrappedColored(fonts.body, color::muted, ("From " + PathUtf8(import_source_)).c_str());
            TextWrappedColored(fonts.body, color::muted, ("Into " + PathUtf8(SaveFolder())).c_str());
            TextWrappedColored(fonts.body, color::muted,
                               "It replaces the game's save there; the current one is backed up first.");
            ImGui::Dummy({0, Dp(8)});
            if (PrimaryButton("import_confirm", "Import", Dp(140, 42), true, Icon::Copy))
            {
                ImGui::CloseCurrentPopup();
                ImportDolphinSave();
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("import_cancel", "Cancel", Dp(100, 42)) || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // A path on one line: when too long, its start gives way ("...lib/Wii").
    static void DrawPath(ImDrawList* draw, ImFont* font, ImVec2 pos, float width, const ImVec4& tint,
                         const std::string& text)
    {
        const float size = FontSize(font);
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= width)
        {
            draw->AddText(font, size, pos, Col(tint), text.c_str());
            return;
        }
        const float dots = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
        std::size_t start = 0;
        while (start < text.size()
               && font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str() + start).x + dots > width)
        {
            ++start;
            while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
        }
        draw->AddText(font, size, pos, Col(tint), ("..." + text.substr(start)).c_str());
    }

    // One line of text, cut with an ellipsis to `width`.
    static void DrawClipped(ImDrawList* draw, ImFont* font, ImVec2 pos, float width, const ImVec4& tint,
                            const std::string& text)
    {
        ImGui::PushFont(font);
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        const ImVec2 max{pos.x + width, pos.y + FontSize(font)};
        ImGui::RenderTextEllipsis(draw, pos, max, max.x, max.x, text.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    bool InLibrary(const fs::path& path) const
    {
        const auto key = ImageCache::Key(path);
        return std::any_of(library_.begin(), library_.end(),
                           [&](const LibraryImage& image) { return ImageCache::Key(image.path) == key; });
    }

    // One row of the images table: an image of the game folder, or the chosen
    // image when the folder's scan does not list it: a file added with Choose
    // file (outside the folder), or one that has gone.
    struct ImageRow
    {
        LibraryImage image;
        bool chosen = false;
        bool missing = false;
        bool outside = false;
        bool checking = false;
    };

    // The list belongs to a finished scan of exactly the current folder setting.
    bool FolderScanned() const
    {
        return !library_scanning_ && scanned_folder_ == draft_.folder && scanned_subfolders_ == draft_.folder_subfolders;
    }

    std::vector<ImageRow> ImageRows() const
    {
        std::vector<ImageRow> rows;
        const std::string selection = DiscSelection();
        const std::string chosen = selection.empty() ? std::string() : ImageCache::Key(PathFromUtf8(selection));
        const bool inside = !selection.empty() && InFolder(PathFromUtf8(selection), GameFolder());
        if (!selection.empty() && (!inside || (FolderScanned() && !InLibrary(PathFromUtf8(selection)))))
        {
            ImageRow row;
            row.chosen = true;
            row.outside = !inside;
            row.missing = !PinnedMissing().empty();
            row.checking = pending_;
            row.image.path = PathFromUtf8(selection);
            const bool current = check_.selection == selection;
            if (const auto info = PinnedInfo())
            {
                row.image.probe.kind = DiscProbe::Kind::Charged;
                row.image.probe.info = *info;
            }
            else if (current && !row.missing) row.image.probe.error = check_.error;
            if (current) row.image.size = check_.size;
            // A missing image keeps the size it had when it was last seen.
            if (const auto* entry = image_cache_.Last(row.image.path); !row.image.size && entry)
                row.image.size = entry->size;
            rows.push_back(std::move(row));
        }
        for (const auto* image : ArrangeImages(library_, library_sort_, library_descending_))
        {
            ImageRow row;
            row.image = *image;
            row.chosen = !chosen.empty() && ImageCache::Key(image->path) == chosen;
            row.checking = row.chosen && pending_;
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // The game images: the game folder and one table of the images in it
    // (and the chosen one); clicking an image makes it the one Play starts.
    void GameImagesCard()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##images", "Game images", "Click the image you want to play.", Icon::Disc);
        GameFolderPicker();
        ImGui::Dummy({0, Dp(6)});

        // The folder's images come from a fresh scan; until it is done the
        // table shows only a file added with Choose file.
        const bool scanned = FolderScanned();
        const bool missing_folder = scanned && !library_available_.empty() && !library_available_[0];
        if (!ImageRows().empty()) ImageTable();
        if (!scanned)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float line = Dp(44);
            Spinner({at.x + Dp(22), at.y + line * 0.5f}, Dp(7), Col(color::info));
            const auto* progress = library_progress_.get();
            const std::string text = !progress || progress->listing ? "Reading your game folder..."
                : "Checking images: " + std::to_string(progress->checked.load()) + " of "
                    + std::to_string(progress->found.load()) + "...";
            DrawLabel(ImGui::GetWindowDrawList(), fonts.body,
                      {at.x + Dp(44), at.y + (line - FontSize(fonts.body)) * 0.5f}, Col(color::muted), text.c_str());
            ImGui::Dummy({ImGui::GetContentRegionAvail().x, line});
        }
        else if (library_.empty())
            Callout(missing_folder
                        ? "Your game folder is not there. Create it and put your Mario Strikers Charged images (ISO "
                          "or RVZ) into it, connect its drive, or select another folder."
                        : "No Mario Strikers Charged images in your game folder. Put them into it, select another "
                          "folder, or choose a file below.",
                    color::info, Icon::Info);

        // Footer: what the table leaves out, and a file from anywhere else.
        ImGui::Dummy({0, Dp(4)});
        std::string footer;
        if (library_other_games_ > 0)
            footer = std::to_string(library_other_games_)
                + (library_other_games_ == 1 ? " other game is" : " other games are") + " not listed.";
        if (library_limited_)
            footer += std::string(footer.empty() ? "" : " ")
                + "Only the first 2000 image files were looked at; select a folder closer to your images.";
        const char* choose = candidate_pending_ ? "Checking the file..." : "Choose file...";
        const ImVec2 button{GhostWidth(choose, Icon::Disc), Dp(34)};
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (!footer.empty())
        {
            const float wrap = width - button.x - Dp(16);
            const float text = fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, wrap, footer.c_str()).y;
            ImGui::GetWindowDrawList()->AddText(fonts.caption, FontSize(fonts.caption),
                                                {at.x + Dp(2), at.y + std::max(0.0f, (button.y - text) * 0.5f)},
                                                Col(color::dim), footer.c_str(), nullptr, wrap);
        }
        ImGui::SetCursorScreenPos({at.x + width - button.x, at.y});
        if (GhostButton("choose_file", choose, button,
                        config_ok_ && !options_.launch.disc && !DialogActive() && !candidate_pending_, Icon::Disc))
            Browse(ExistingFolder(GameFolder().path));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play an image that is not in your game folder.\n"
                              "You can also drop an image onto this window.");
        EndCard();
    }

    // The game folder as one picker: click it (or Change...) to select another
    // folder; it shows what the folder holds, its Subfolders switch and refresh.
    void GameFolderPicker()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = Dp(60);
        const ImVec2 max{pos.x + width, pos.y + height};
        const float center = pos.y + height * 0.5f;
        bool browsing;
        { std::lock_guard<std::mutex> lock(folder_dialog_->mutex); browsing = folder_dialog_->active; }
        const bool can_select = !browsing && config_ok_;

        // Controls on the right, from the edge inwards.
        const char* change = "Change...";
        const char* subfolders = "Subfolders";
        const float change_w = TextSize(fonts.body, change).x + Dp(32);
        const float change_x = max.x - Dp(12) - change_w;
        const float refresh_x = change_x - Dp(8) - Dp(34);
        const float divider_x = refresh_x - Dp(12);
        const float toggle_x = divider_x - Dp(12) - Dp(48);
        const float label_x = toggle_x - Dp(10) - TextSize(fonts.caption, subfolders).x;

        // The folder itself is a button too: it opens the folder dialog.
        ImGui::SetCursorScreenPos(pos);
        ImGui::BeginDisabled(!can_select);
        const bool picked = ImGui::InvisibleButton("##game_folder", {label_x - Dp(14) - pos.x, height});
        ImGui::EndDisabled();
        const float hover = Animate(ImGui::GetID("##folder_hover"), ImGui::IsItemHovered() && can_select, 12.0f);
        draw->AddRectFilled(pos, max, Col(Mix(Mix(color::field, color::card, 0.45f), color::field, hover)), Dp(12));
        draw->AddRect(pos, max, Col(Mix(color::border, color::accent, 0.45f * hover)), Dp(12), 0, Dp(1));
        const ImVec2 box{pos.x + Dp(12), center - Dp(18)};
        draw->AddRectFilled(box, {box.x + Dp(36), box.y + Dp(36)}, Col(color::accent, 0.12f + 0.06f * hover), Dp(10));
        DrawIcon(draw, Icon::Folder, {box.x + Dp(18), box.y + Dp(18)}, Dp(18), Col(color::accent));

        const fs::path path = GameFolder().path;
        const bool scanned = FolderScanned();
        const bool unavailable = scanned && !library_available_.empty() && !library_available_[0];
        std::string status;
        ImVec4 tint = color::muted;
        if (!scanned)
        {
            status = "Reading...";
            tint = color::info;
        }
        else if (unavailable)
        {
            status = "Not found";
            tint = color::warning;
        }
        else
        {
            const auto count = std::count_if(library_.begin(), library_.end(), [](const LibraryImage& image) {
                return image.probe.kind == DiscProbe::Kind::Charged;
            });
            status = count == 0 ? "No Mario Strikers Charged images" : count == 1 ? "1 image"
                                                                                   : std::to_string(count) + " images";
        }
        const float text_x = box.x + Dp(36 + 14), text_w = std::max(Dp(80), label_x - Dp(18) - text_x);
        const float lines = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
        const float top = center - lines * 0.5f;
        DrawPath(draw, fonts.label, {text_x, top}, text_w, color::text, PathUtf8(path));
        DrawClipped(draw, fonts.caption, {text_x, top + FontSize(fonts.label) + Dp(3)}, text_w, tint, status);
        if (picked) BrowseFolder(FolderPurpose::GameFolder, path);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nClick to select another game folder.\nYou can also drop a folder onto this window.",
                              PathUtf8(path).c_str());

        DrawLabel(draw, fonts.caption, {label_x, center - FontSize(fonts.caption) * 0.5f}, Col(color::muted),
                  subfolders);
        ImGui::SetCursorScreenPos({toggle_x, center - Dp(14)});
        const auto before = GameFolder();
        if (Toggle("subfolders", &draft_.folder_subfolders)) GameFolderChanged(before);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Also list the images in its subfolders (%d levels down).", kLibraryDepth);
        draw->AddRectFilled({divider_x, center - Dp(14)}, {divider_x + Dp(1), center + Dp(14)}, Col(color::border));
        ImGui::SetCursorScreenPos({refresh_x, center - Dp(17)});
        if (GhostButton("refresh_library", "", Dp(34, 34), config_ok_ && !library_scanning_, Icon::Refresh))
        {
            StartLibraryScan();
            if (!DiscSelection().empty() && !pending_) CheckDisc();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look through the folder again.");
        ImGui::SetCursorScreenPos({change_x, center - Dp(17)});
        if (SecondaryButton("change_folder", change, {change_w, Dp(34)}, can_select))
            BrowseFolder(FolderPurpose::GameFolder, path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select another game folder.");
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({width, height});
    }

    enum ImageColumn { ColumnChosen, ColumnGame, ColumnVersion, ColumnFile, ColumnOpen };

    void ImageTable()
    {
        const auto& fonts = CurrentFonts();
        // Narrow windows give the banner less room and move the file's size
        // and format under its name.
        const bool narrow = ImGui::GetContentRegionAvail().x < Dp(660);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(7, 0));
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4{0, 0, 0, 0});
        // Rows draw their own rounded highlight.
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4{0, 0, 0, 0});
        if (ImGui::BeginTable("##images_table", narrow ? 4 : 5,
                              ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX
                                  | ImGuiTableFlags_NoClip))
        {
            const auto fixed = ImGuiTableColumnFlags_WidthFixed;
            ImGui::TableSetupColumn("##chosen", fixed | ImGuiTableColumnFlags_NoSort, Dp(30), ColumnChosen);
            ImGui::TableSetupColumn("GAME", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColumnGame);
            ImGui::TableSetupColumn("VERSION", fixed | ImGuiTableColumnFlags_DefaultSort, Dp(narrow ? 100 : 112),
                                    ColumnVersion);
            if (!narrow)
                ImGui::TableSetupColumn("FILE", fixed | ImGuiTableColumnFlags_PreferSortDescending, Dp(66), ColumnFile);
            ImGui::TableSetupColumn("##open", fixed | ImGuiTableColumnFlags_NoSort, Dp(32), ColumnOpen);
            if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0)
            {
                const auto& spec = specs->Specs[0];
                library_sort_ = spec.ColumnUserID == ColumnGame   ? LibrarySort::Name
                              : spec.ColumnUserID == ColumnFile ? LibrarySort::Size
                                                                : LibrarySort::Release;
                library_descending_ = spec.SortDirection == ImGuiSortDirection_Descending;
                specs->SpecsDirty = false;
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers, Dp(24));
            ImGui::PushFont(fonts.caption);
            ImGui::PushStyleColor(ImGuiCol_Text, color::dim);
            for (int column = 0; column < ImGui::TableGetColumnCount(); ++column)
            {
                ImGui::TableSetColumnIndex(column);
                ImGui::TableHeader(ImGui::TableGetColumnName(column));
            }
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (const ImGuiTable* table = ImGui::GetCurrentTable())
            {
                const float y = ImGui::GetItemRectMax().y + Dp(3);
                ImGui::GetWindowDrawList()->AddRectFilled({table->WorkRect.Min.x, y}, {table->WorkRect.Max.x, y + Dp(1)},
                                                          Col(color::border));
            }
            for (const auto& row : ImageRows()) ImageTableRow(row, narrow);
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    void ImageTableRow(const ImageRow& row, bool narrow)
    {
        const auto& fonts = CurrentFonts();
        const auto& image = row.image;
        const auto& info = image.probe.info;
        const bool charged = image.probe.kind == DiscProbe::Kind::Charged;
        const ImageState state = StateOf(image);
        const bool playable = charged && !row.missing && state == ImageState::Playable;
        const bool can_choose = playable && !row.chosen && config_ok_ && !options_.launch.disc;
        const std::string path = PathUtf8(image.path), name = PathUtf8(image.path.filename());

        // What the row says beyond its details: why it cannot be played (in
        // short; the tooltip has the full reason), or a remark.
        std::string note, reason;
        ImVec4 note_tint = color::dim;
        if (row.chosen && options_.launch.disc)
        {
            note = "From the command line: used for this run only.";
            note_tint = color::info;
        }
        else if (row.checking) { note = "Checking the image..."; note_tint = color::info; }
        else if (row.missing)
        {
            note = check_.folder_missing ? "Missing: its folder is not there. Connect its drive, or click to locate it."
                                         : "Missing: it was moved, renamed or deleted. Click to locate it.";
            note_tint = color::warning;
        }
        else if (state == ImageState::Unsupported)
        {
            const auto* release = FindRelease(info);
            bool known_id = false;
            for (std::size_t n = 0; n < ReleaseCount(); ++n) known_id = known_id || info.game_id == ReleaseAt(n).game_id;
            note = release ? "Not supported by this build yet." : known_id ? "This revision is not supported yet."
                                                                           : "Not supported yet.";
            reason = UnplayableReason(info, release) + "\nThis build runs " + PlayableReleasesText() + ".";
            note_tint = color::warning;
        }
        else if (state == ImageState::Unreadable && !image.probe.error.empty())
        {
            note = "Cannot be read: damaged or incomplete file.";
            reason = image.probe.error;
            note_tint = color::danger;
        }
        else if (const auto* release = charged ? FindRelease(info) : nullptr; release && !ReleaseNote(*release).empty())
            note = ReleaseNote(*release);
        if (row.outside && note.empty()) note = "Chosen file outside your game folder.";

        const float label = FontSize(fonts.label), caption = FontSize(fonts.caption);
        const ImVec2 banner = narrow ? Dp(78, 26) : Dp(128, 43);
        const float text_block = label + Dp(4) + caption + (note.empty() ? 0.0f : Dp(4) + caption);
        const float height = std::max(banner.y, text_block) + Dp(18);
        // Rows that cannot be played step back a little.
        const float fade = playable || row.chosen ? 1.0f : 0.6f;
        ImGui::PushID(path.c_str());
        ImGui::TableNextRow(ImGuiTableRowFlags_None, height);

        // The whole row is one button: it chooses the image (or locates a missing one).
        ImGui::TableSetColumnIndex(ColumnChosen);
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Selectable("##row", false,
                                               ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                               {0, height});
        const bool hovered = ImGui::IsItemHovered();
        const float hover = Animate(ImGui::GetID("##hover"), hovered && (can_choose || row.missing), 14.0f);
        if (clicked)
        {
            if (row.missing && !options_.launch.disc) Locate();
            else if (can_choose) UseImage(image.path, info, image.size);
        }
        if (hovered)
        {
            std::string tip = path;
            if (row.chosen && !row.missing) tip += "\nPlay starts this image.";
            else if (can_choose) tip += "\nClick to play this image.";
            else if (!reason.empty()) tip += "\n" + reason;
            else if (!note.empty() && row.missing) tip += "\n" + note;
            ImGui::SetTooltip("%s", tip.c_str());
        }
        auto* draw = ImGui::GetWindowDrawList();
        const ImGuiTable* table = ImGui::GetCurrentTable();
        const float left = table ? table->WorkRect.Min.x : cell.x;
        const float right = table ? table->WorkRect.Max.x : cell.x + ImGui::GetContentRegionAvail().x;
        // A rounded highlight for the chosen row and the one under the mouse,
        // thin separators elsewhere.
        const ImVec2 box_min{left, cell.y + Dp(3)}, box_max{right, cell.y + height - Dp(3)};
        if (row.chosen)
        {
            const ImVec4 tint = row.missing ? color::warning : color::accent;
            draw->AddRectFilled(box_min, box_max, Col(tint, 0.08f + 0.04f * hover), Dp(12));
            draw->AddRect(box_min, box_max, Col(tint, 0.45f), Dp(12), 0, Dp(1.2f));
        }
        else
        {
            if (hover > 0.0f) draw->AddRectFilled(box_min, box_max, Col(color::text, 0.05f * hover), Dp(12));
            draw->AddRectFilled({left + Dp(12), cell.y + height - Dp(1)}, {right - Dp(12), cell.y + height},
                                Col(color::border, 0.6f));
        }
        const float middle = cell.y + height * 0.5f;
        // A radio button: filled for the chosen image, empty for others it could be.
        {
            const ImVec2 center{cell.x + Dp(15), middle};
            const float radius = Dp(8);
            if (row.chosen)
            {
                const ImVec4 tint = row.missing ? color::warning : color::accent;
                draw->AddCircleFilled(center, radius, Col(tint, 0.18f));
                draw->AddCircle(center, radius, Col(tint), 0, Dp(1.6f));
                draw->AddCircleFilled(center, radius * 0.45f, Col(tint));
            }
            else
                draw->AddCircle(center, radius, Col(playable ? Mix(color::muted, color::text, hover) : color::dim,
                                                    playable ? 1.0f : 0.35f),
                                0, Dp(1.4f));
        }

        // Game: banner, title, file name and the note.
        ImGui::TableSetColumnIndex(ColumnGame);
        const float game_x = ImGui::GetCursorScreenPos().x;
        const float game_w = ImGui::GetContentRegionAvail().x;
        const ImVec2 banner_min{game_x, middle - banner.y * 0.5f};
        DrawBanner(draw, banner_min, {banner_min.x + banner.x, banner_min.y + banner.y}, charged ? info.game_id : "",
                   row.missing ? 0.35f : fade);
        const float text_x = game_x + banner.x + Dp(14), text_w = std::max(Dp(60), game_x + game_w - text_x);
        float y = middle - text_block * 0.5f;
        DrawClipped(draw, fonts.label, {text_x, y}, text_w, Mix(color::muted, color::text, row.missing ? 0.0f : fade),
                    charged ? ImageTitle(image) : name);
        y += label + Dp(4);
        std::string file = charged ? name : PathUtf8(image.path.parent_path());
        if (narrow && charged && image.size)
            file = FileSizeText(image.size) + "  \xC2\xB7  " + info.format + "  \xC2\xB7  " + file;
        if (charged) DrawClipped(draw, fonts.caption, {text_x, y}, text_w, color::dim, file);
        else DrawPath(draw, fonts.caption, {text_x, y}, text_w, color::dim, file);
        y += caption + Dp(4);
        if (!note.empty()) DrawClipped(draw, fonts.caption, {text_x, y}, text_w, note_tint, note);

        // Version: region, then revision and game ID. File: size, then format.
        const float detail_top = middle - (caption + Dp(4) + caption) * 0.5f;
        const ImVec4 strong = Mix(color::muted, color::text, row.missing ? 0.0f : fade);
        if (charged)
        {
            ImGui::TableSetColumnIndex(ColumnVersion);
            const float x = ImGui::GetCursorScreenPos().x, w = ImGui::GetContentRegionAvail().x;
            DrawRegionFlag(draw, {x, detail_top + Dp(1.5f)}, Dp(11), RegionLetter(info.game_id));
            DrawClipped(draw, fonts.caption, {x + Dp(22), detail_top}, w - Dp(22), strong, DiscRegionName(info.game_id));
            DrawClipped(draw, fonts.caption, {x, detail_top + caption + Dp(4)}, w, color::dim,
                        "Rev. " + std::to_string(unsigned(info.revision)) + "  \xC2\xB7  " + info.game_id);
        }
        if (!narrow)
        {
            ImGui::TableSetColumnIndex(ColumnFile);
            const float x = ImGui::GetCursorScreenPos().x, w = ImGui::GetContentRegionAvail().x;
            if (image.size) DrawClipped(draw, fonts.caption, {x, detail_top}, w, strong, FileSizeText(image.size));
            if (charged) DrawClipped(draw, fonts.caption, {x, detail_top + caption + Dp(4)}, w, color::dim, info.format);
        }
        // Open the image's folder.
        if (!row.missing)
        {
            ImGui::TableSetColumnIndex(narrow ? 3 : ColumnOpen);
            ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, middle - Dp(16)});
            if (GhostButton("open", "", Dp(32, 32), true, Icon::Folder)) OpenPath(image.path.parent_path());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the folder with this image.");
        }
        ImGui::PopID();
    }

    // Makes this image the one Play starts and says so.
    void UseImage(const fs::path& path, const DiscInfo& info, std::uintmax_t size)
    {
        Pin(path, info, size);
        Notice("Play now starts " + PathUtf8(path.filename()) + " (" + RegionRevision(info) + ").",
               NoticeKind::Success);
    }

    void LanguageControl()
    {
        const auto& release = Release();
        std::vector<const char*> values{"auto"}, labels{"Auto"};
        for (std::size_t i = 0; i < release.language_count; ++i)
        {
            values.push_back(release.languages[i].value);
            labels.push_back(release.languages[i].label);
        }
        int selected = -1;
        for (std::size_t i = 0; i < values.size(); ++i) if (draft_.language == values[i]) selected = int(i);
        if (Segmented("##language_choice", &selected, labels, ImGui::GetContentRegionAvail().x))
        { draft_.language = values[selected]; dirty_ = true; }
        const auto choice = ChooseLanguage(release, draft_.language, pc_languages_);
        if (selected < 0)
            TextWrappedColored(CurrentFonts().caption, color::warning,
                               ("The saved language is not available in this version; the game uses Automatic ("
                                + std::string(choice.language->label) + "). Choose one above.").c_str());
        else if (choice.automatic)
            TextWrappedColored(CurrentFonts().caption, color::muted,
                               ("Automatic: " + std::string(choice.language->label) + ".").c_str());
    }

    void DisplayPage()
    {
        PageHeader("Display", "How the game window opens and how its picture is drawn.");
        if (draft_.performance_mode)
        {
            Chip("Performance mode is on, so antialiasing stays off.", color::info, Icon::Display);
            ImGui::Dummy({0, Dp(4)});
        }
        const auto run = EffectiveLaunch().settings;
        if (options_.launch.size || options_.launch.fullscreen || options_.launch.aspect || options_.launch.frame_rate)
        {
            const std::string text = "This run: " + std::to_string(run.width) + " x " + std::to_string(run.height) + ", "
                + run.aspect + (run.fullscreen ? ", fullscreen" : ", windowed") + ", " + FrameRateLabel(run.frame_rate)
                + " (command line, not saved)";
            Chip(text.c_str(), color::info, Icon::Info);
            ImGui::Dummy({0, Dp(2)});
        }
        const auto hotkeys = DraftHotkeys();
        const auto key = [&](HotkeyIndex hotkey) { return platform::HotkeyLabel(hotkeys[hotkey]); };

        BeginCard("##window", "Screen", nullptr, Icon::Display);
        if (BeginRows("##window_rows"))
        {
            const std::string mode_note = std::string("Fullscreen covers the whole display, at the resolution below.")
                + (hotkeys[HotkeyFullscreen].key ? " " + key(HotkeyFullscreen) + " switches while playing."
                                                 : std::string());
            Row("Mode", mode_note.c_str());
            int mode = draft_.fullscreen ? 1 : 0;
            if (Segmented("##mode", &mode, {"Windowed", "Fullscreen"}, ImGui::GetContentRegionAvail().x))
            { draft_.fullscreen = mode == 1; dirty_ = true; }

            // One resolution row: the window's size, or the screen's in fullscreen.
            if (!draft_.fullscreen)
            {
                Row("Resolution", draft_.remember_window
                    ? "The window's size, from those your graphics card offers for this display. The game also "
                      "keeps the size and place you leave its window at."
                    : "The window's size, from those your graphics card offers for this display.");
                WindowSizeControl();
            }
            else
            {
                Row("Resolution", "The screen's resolution. Desktop keeps the display's own; another switches the "
                    "display to it while the game runs.");
                FullscreenResolutionControl();
            }

            if (displays_.size() > 1 || draft_.monitor > 0)
            {
                Row("Display", "Where the game window opens.");
                DisplayChoice();
            }
            EndRows();
        }
        EndCard();

        BeginCard("##picture", "Picture", nullptr, Icon::Sliders);
        if (BeginRows("##picture_rows"))
        {
            const std::string aspect_note = "16:9 is the game's widescreen layout and 4:3 its original one; both keep "
                "their shape. Stretch fills the whole screen."
                + (hotkeys[HotkeyStretch].key ? " " + key(HotkeyStretch) + " switches while playing." : std::string());
            Row("Aspect ratio", aspect_note.c_str());
            const char* values[] = {"16:9", "4:3", "stretch"};
            int aspect = -1;
            for (int i = 0; i < 3; ++i) if (draft_.aspect == values[i]) aspect = i;
            if (Segmented("##aspect", &aspect, {"16:9", "4:3", "Stretch"}, ImGui::GetContentRegionAvail().x))
            { draft_.aspect = values[aspect]; dirty_ = true; }
            AspectNote(aspect < 0);
            if (draft_.aspect == "16:9")
            {
                Row("Wider view", "In matches on a screen wider or taller than 16:9, shows more of the stadium "
                    "instead of bars, with the HUD in the 16:9 middle. Menus keep their 16:9 picture.");
                if (Toggle("##wider_view", &draft_.wider_view)) dirty_ = true;
            }

            std::string resolution_note = "Full draws the game at the resolution above, the sharpest. Native is the "
                "Wii's 640 x 448; 2x-4x are its multiples, for slower graphics cards.";
            if (hotkeys[HotkeyResolutionUp].key || hotkeys[HotkeyResolutionDown].key)
                resolution_note += " " + key(hotkeys[HotkeyResolutionUp].key ? HotkeyResolutionUp : HotkeyResolutionDown)
                    + (hotkeys[HotkeyResolutionUp].key && hotkeys[HotkeyResolutionDown].key
                           ? " and " + key(HotkeyResolutionDown) + " change it" : std::string(" changes it"))
                    + " while playing.";
            Row("3D resolution", resolution_note.c_str());
            const char* resolutions[] = {"window", "native", "2x", "3x", "4x"};
            int resolution = 0;
            for (int i = 0; i < 5; ++i) if (draft_.resolution == resolutions[i]) resolution = i;
            if (Segmented("##resolution", &resolution, {"Full", "Native", "2x", "3x", "4x"},
                          ImGui::GetContentRegionAvail().x))
            { draft_.resolution = resolutions[resolution]; dirty_ = true; }
            ResolutionNote();

            Row("Antialiasing", "Smooths jagged edges. The stronger kinds cost more GPU time.");
            AntialiasingKindChoice();

            Row("Picture style", "Modern draws the game's picture crisp, straight into the window. Original TV adds "
                "the Wii's flicker filter and a TV's softness.");
            int style = draft_.picture == "soft" ? 1 : 0;
            if (Segmented("##style", &style, {"Modern", "Original TV"}, ImGui::GetContentRegionAvail().x))
            {
                if (style == 1 && draft_.picture != "soft") modern_picture_ = draft_.picture;
                draft_.picture = style == 1 ? "soft" : modern_picture_;
                dirty_ = true;
            }
            if (draft_.picture == "soft")
            {
                BeginSubRows();
                Row("TV border", "The empty border a Wii sends around its picture, which a TV hides.");
                if (Toggle("##tv_border", &draft_.tv_border)) dirty_ = true;
                EndSubRows();
            }
            else if (draft_.resolution != "window")
            {
                BeginSubRows();
                Row("Upscaling", "How a 3D resolution other than Full reaches the resolution above. Sharp keeps "
                    "pixels square.");
                int sharp = draft_.picture == "clean" ? 0 : 1;
                if (Segmented("##upscaling", &sharp, {"Smooth", "Sharp"}, ImGui::GetContentRegionAvail().x))
                { draft_.picture = sharp ? "sharp" : "clean"; dirty_ = true; }
                EndSubRows();
            }
            EndRows();
        }
        EndCard();

        BeginCard("##motion", "Motion", nullptr, Icon::Play);
        if (BeginRows("##motion_rows"))
        {
            Row("VSync", "Synchronizes frames with your display to prevent tearing.");
            if (Toggle("##vsync", &draft_.vsync)) dirty_ = true;

            Row("Frame rate", "Original is the console's 60 FPS. Higher rates draw more frames for smoother motion "
                "while the game keeps its own speed; Display follows your display's refresh rate.");
            FrameRateChoice();
            EndRows();
        }
        EndCard();

        BeginCard("##frame_rate", "Performance overlay", "FPS, system GPU and CPU usage, and network latency.", Icon::Info);
        if (BeginRows("##frame_rate_rows"))
        {
            Row("On screen", "A compact row in a corner, also in fullscreen. Latency shows N/A until online play is available.");
            if (Toggle("##fps_overlay", &draft_.fps_overlay)) dirty_ = true;

            Row("Corner", "Where the performance overlay sits.");
            FpsCornerChoice();
            EndRows();
        }
        EndCard();
    }

    // Whether the game's picture fills its window or display: stretched, or
    // with the wider view.
    bool PictureFills() const
    {
        return draft_.aspect == "stretch" || (draft_.aspect == "16:9" && draft_.wider_view);
    }

    // The screen the game's picture fills: the display (fullscreen) or the
    // window, in pixels of its shape; {} when unknown.
    ShapeSize PictureScreen() const
    {
        if (!draft_.fullscreen) return {draft_.width, draft_.height};
        if (const auto chosen = ParseResolution(draft_.fullscreen_resolution); chosen.width) return chosen;
        const SDL_DisplayMode* mode = StartDisplayMode();
        if (!mode || mode->w <= 0 || mode->h <= 0) return {};
        const float density = mode->pixel_density > 0 ? mode->pixel_density : 1.0f;
        return {int(std::lround(mode->w * density)), int(std::lround(mode->h * density))};
    }

    // What the aspect ratio does on the display the game opens on.
    void AspectNote(bool unknown)
    {
        std::string note;
        const auto screen = PictureScreen();
        const bool wide = WidescreenLayout(draft_.aspect, screen.width, screen.height);
        const int aw = wide ? 16 : 4, ah = wide ? 9 : 3;
        const std::string shape = wide ? "16:9" : "4:3";
        const std::string size = std::to_string(screen.width) + " x " + std::to_string(screen.height);
        if (unknown)
            note = draft_.aspect == "auto"
                ? "Saved value auto: 16:9 or 4:3, whichever is closer to the screen's shape (" + shape
                    + " here). Choose one above to change it."
                : "Saved value " + draft_.aspect + " is not supported by the game. Choose one above.";
        else if (draft_.aspect == "stretch")
        {
            const int percent = StretchPercent(screen.width, screen.height, aw, ah);
            if (!draft_.fullscreen)
                note = "Uses the " + shape + " layout and fills the window at any size, stretching the picture with "
                    "it.";
            else if (screen.width <= 0)
                note = "Uses the " + shape + " layout and fills the whole display.";
            else if (std::abs(percent) < 1)
                note = "Uses the " + shape + " layout, which fills this " + size + " display exactly.";
            else
                note = "Uses the " + shape + " layout and fills this " + size + " display; the picture is "
                    + std::to_string(std::abs(percent)) + "% " + (percent > 0 ? "wider" : "narrower") + " than "
                    + shape + ".";
        }
        else if (PictureFills() && !draft_.fullscreen)
            note = "The window can take any shape: in matches the wider view fills it with more of the stadium.";
        else if (PictureFills())
        {
            const auto bars = BarsAround(screen.width, screen.height, aw, ah);
            note = bars.size <= 1 || screen.width <= 0 ? "Fills the display."
                : "In matches, fills this " + size + " display with more of the stadium "
                    + (bars.sides ? "at the sides." : "above and below.") + " Menus keep 16:9.";
        }
        else if (!draft_.fullscreen)
            note = "The window keeps the " + shape + " shape when you resize it, so the picture fills it. "
                "Maximized on a screen of another shape, it shows bars.";
        else if (screen.width > 0)
        {
            const auto bars = BarsAround(screen.width, screen.height, aw, ah);
            note = bars.size <= 1 ? "Fills this " + size + " display exactly."
                : "On this " + size + " display: bars of " + std::to_string(bars.size) + " pixels "
                    + (bars.sides ? "left and right." : "above and below.")
                    + (wide ? " Stretch or the wider view fills it." : " Stretch fills it.");
        }
        if (!note.empty())
            TextWrappedColored(CurrentFonts().caption, unknown ? color::warning : color::muted, note.c_str());
    }

    // The 3D resolution's size where the game opens.
    void ResolutionNote()
    {
        const auto screen = PictureScreen();
        if (screen.width <= 0 || screen.height <= 0) return;
        const bool wide = WidescreenLayout(draft_.aspect, screen.width, screen.height);
        const auto picture = PictureSize(screen.width, screen.height, wide ? 16 : 4, wide ? 9 : 3, PictureFills());
        const int width = RenderWidth(draft_.resolution, picture.width);
        const int height = RenderHeight(draft_.resolution, picture.height);
        const std::string where = draft_.fullscreen ? "this screen" : "this window";
        std::string note = std::string(RenderResolutionLabel(draft_.resolution)) + ": " + std::to_string(width) + " x "
            + std::to_string(height);
        if (RenderScale(draft_.resolution) == 0.0f) note += " on " + where + ".";
        else
        {
            const long long pixels = 1LL * picture.width * picture.height, drawn = 1LL * width * height;
            note += std::string(", ") + (drawn < pixels * 9 / 10 ? "fewer pixels than"
                                         : drawn > pixels * 11 / 10 ? "more pixels than" : "about as many pixels as")
                + " the picture on " + where + ".";
        }
        TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
    }

    // The desktop mode of the display the game opens on (display.monitor; the
    // default display when none is chosen), or null.
    const SDL_DisplayMode* StartDisplayMode() const
    {
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (draft_.monitor > 0 && draft_.monitor <= int(display_ids_.size()))
            display = display_ids_[std::size_t(draft_.monitor - 1)];
        return display ? SDL_GetDesktopDisplayMode(display) : nullptr;
    }

    // The refresh rate of the display the game opens on (display.monitor), 0
    // when unknown.
    double StartDisplayRefresh() const
    {
        const SDL_DisplayMode* mode = StartDisplayMode();
        if (!mode) return 0.0;
        if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
            return double(mode->refresh_rate_numerator) / double(mode->refresh_rate_denominator);
        return KnownRefreshRate(mode->refresh_rate) ? double(mode->refresh_rate) : 0.0;
    }

    static std::string FrameRateLabel(const std::string& value)
    {
        if (value == "original") return "original frame rate";
        if (value == "display") return "display refresh rate";
        if (value == "unlimited") return "unlimited FPS";
        return value + " FPS";
    }

    void FrameRateChoice()
    {
        static constexpr const char* values[] = {"original", "display", "120", "144", "unlimited"};
        int selected = -1;
        for (int i = 0; i < 5; ++i) if (draft_.frame_rate == values[i]) selected = i;
        if (Segmented("##frame_rate", &selected, {"Original", "Display", "120", "144", "Unlimited"},
                      ImGui::GetContentRegionAvail().x))
        { draft_.frame_rate = values[selected]; dirty_ = true; }
        const auto rate = ParseFrameRate(draft_.frame_rate);
        if (!rate) return;
        std::string note;
        if (selected < 0) note = "Saved value: " + draft_.frame_rate + " FPS. ";
        if (rate->kind != FrameRate::Kind::Original)
        {
            const double display = StartDisplayRefresh();
            const auto pacing = DecideFramePacing(*rate, display, draft_.vsync);
            const auto hz = [](double value) { return std::to_string(int(std::lround(value))); };
            if (pacing.vsync_paces && KnownRefreshRate(display))
                note += "VSync holds it to this display's " + hz(display) + " Hz.";
            else if (KnownRefreshRate(display) && pacing.target_hz > display + 0.5)
                note += "This display refreshes at " + hz(display) + " Hz, so it cannot show every frame.";
            else if (pacing.target_hz == 0.0)
                note += "No limit: the game draws as fast as your computer allows.";
            else if (KnownRefreshRate(display))
                note += "This display refreshes at " + hz(display) + " Hz.";
        }
        if (!note.empty()) TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
    }

    void AntialiasingKindChoice()
    {
        const mscharged::AntialiasingChoice* selected = nullptr;
        for (const auto& choice : kAntialiasingChoices)
            if (draft_.antialiasing == choice.value) selected = &choice;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##antialiasing", selected ? selected->label : draft_.antialiasing.c_str()))
        {
            for (const auto& choice : kAntialiasingChoices)
                if (ImGui::Selectable(choice.label, selected == &choice))
                { draft_.antialiasing = choice.value; dirty_ = true; }
            ImGui::EndCombo();
        }
        // The chosen kind's note; the launcher reads only known values.
        for (const auto& choice : kAntialiasingChoices)
            if (draft_.antialiasing == choice.value)
                TextWrappedColored(CurrentFonts().caption, color::muted, choice.description);
    }

    void FpsCornerChoice()
    {
        static constexpr std::pair<const char*, const char*> corners[] = {
            {"top-left", "Top left"}, {"top-right", "Top right"},
            {"bottom-left", "Bottom left"}, {"bottom-right", "Bottom right"}};
        const char* preview = "Top right";
        for (const auto& [value, name] : corners)
            if (draft_.fps_overlay_corner == value) preview = name;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::BeginDisabled(!draft_.fps_overlay);
        if (ImGui::BeginCombo("##fps_corner", preview))
        {
            for (const auto& [value, name] : corners)
                if (ImGui::Selectable(name, draft_.fps_overlay_corner == value))
                { draft_.fps_overlay_corner = value; dirty_ = true; }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    // Gamepad `slot`'s profile as a two-column table: pad control, then Wii input.
    void GamepadLayoutTooltip(std::size_t slot)
    {
        const auto& profile = draft_.PadProfileOf(slot);
        const auto type = PadType(int(slot));
        const bool builtin = draft_.PadProfileIndex(slot) < 0;
        const auto family = platform::GamepadFamilyOf(type);
        const auto bindings = builtin ? platform::DefaultGamepadBindings(family)
                                      : platform::ParseGamepadBindings(profile.inputs);
        const bool swap = profile.swap_sticks;
        const auto& fonts = CurrentFonts();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(16, 14));
        if (ImGui::BeginTooltip())
        {
            TextColored(fonts.label, color::text,
                        ("Profile: " + profile.name
                         + (builtin ? std::string(" (") + platform::GamepadFamilyName(family) + ")" : std::string()))
                            .c_str());
            TextColored(fonts.caption, color::muted, "Your controller's button for each Wii input. Click to change.");
            const auto row = [&](const std::string& pad, const char* wii) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                TextColored(fonts.body, color::text, pad.c_str());
                ImGui::TableSetColumnIndex(1);
                TextColored(fonts.body, color::accent, wii);
            };
            const auto table = [&](const char* title, const char* id, const auto& rows) {
                ImGui::Dummy({0, Dp(6)});
                TextColored(fonts.caption, color::dim, title);
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 3));
                if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
                {
                    ImGui::TableSetupColumn("pad", ImGuiTableColumnFlags_WidthFixed, Dp(150));
                    ImGui::TableSetupColumn("wii", ImGuiTableColumnFlags_WidthFixed, Dp(200));
                    rows();
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
            };
            table("BUTTONS", "##pad_buttons", [&] {
                for (std::size_t n = 0; n < GamepadActionC; ++n)
                    row(PadBindingLabel(bindings[n], type), kGamepadActions[n].label);
            });
            table("NUNCHUK & MOTION", "##pad_sticks", [&] {
                row(swap ? "Right stick" : "Left stick", "Nunchuk stick");
                for (std::size_t n = GamepadActionC; n < GamepadActionRecenterPointer; ++n)
                    row(PadBindingLabel(bindings[n], type), kGamepadActions[n].label);
                row(profile.gyro_pointer ? "Gyro" : swap ? "Left stick" : "Right stick", "Pointer");
                if (profile.gyro_pointer)
                    row(PadBindingLabel(bindings[GamepadActionRecenterPointer], type),
                        kGamepadActions[GamepadActionRecenterPointer].label);
            });
            ImGui::Dummy({0, Dp(6)});
            TextColored(fonts.caption, color::muted,
                        ("Stick dead zones " + std::to_string(profile.inner_deadzone) + "-"
                         + std::to_string(profile.outer_deadzone) + "%, triggers press at "
                         + std::to_string(profile.trigger_press) + "%"
                         + (profile.gyro_pointer ? ", gyro speed " + std::to_string(profile.gyro_speed) + "%"
                                                 : std::string()))
                            .c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopStyleVar();
    }

    // The controller type whose button names to show: gamepad `slot`, else
    // the first connected one (Xbox names without any).
    SDL_GamepadType PadType(int slot) const
    {
        SDL_Gamepad* pad = slot >= 0 && slot < int(pads_.size()) ? pads_[std::size_t(slot)] : gamepad_;
        return pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_STANDARD;
    }

    // A gamepad input as printed on that kind of controller.
    static std::string PadInputLabel(platform::GamepadInput input, SDL_GamepadType type)
    {
        const bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
        const bool nintendo = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
            || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT
            || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT
            || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
        const bool gamecube = type == SDL_GAMEPAD_TYPE_GAMECUBE;
        const auto pick = [&](const char* xbox, const char* playstation, const char* switch_,
                              const char* cube = nullptr) {
            return std::string(gamecube && cube ? cube : ps ? playstation : nintendo || gamecube ? switch_ : xbox);
        };
        if (input == platform::kGamepadTriggerBase + SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return pick("LT", "L2", "ZL", "L");
        if (input == platform::kGamepadTriggerBase + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return pick("RT", "R2", "ZR", "R");
        if (platform::IsGamepadStickDirection(input))
        {
            static const char* const ways[] = {" up", " down", " left", " right"};
            const int direction = input - platform::kGamepadStickBase;
            return (direction >= platform::RightStickUp ? pick("Right stick", "Right stick", "Right stick", "C-stick")
                                                        : std::string("Left stick"))
                + ways[direction % 4];
        }
        if (input < 0 || input >= SDL_GAMEPAD_BUTTON_COUNT) return "Not set";
        const auto button = SDL_GamepadButton(input);
        if (button <= SDL_GAMEPAD_BUTTON_NORTH)
        {
            switch (SDL_GetGamepadButtonLabelForType(type, button))
            {
            case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
            case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
            case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
            case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
            default: break;
            }
            static const char* const face[] = {"A", "B", "X", "Y"};
            return face[button];
        }
        switch (button)
        {
        case SDL_GAMEPAD_BUTTON_BACK: return pick("Back", "Share", "Minus");
        case SDL_GAMEPAD_BUTTON_GUIDE: return pick("Guide", "PS", "Home");
        case SDL_GAMEPAD_BUTTON_START: return pick("Start", "Options", "Plus", "Start");
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return pick("Left stick click", "L3", "Left stick click");
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return pick("Right stick click", "R3", "Right stick click");
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return pick("LB", "L1", "L");
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return pick("RB", "R1", "R", "Z");
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
        case SDL_GAMEPAD_BUTTON_MISC1: return pick("Share", "Mic", "Capture");
        case SDL_GAMEPAD_BUTTON_MISC3: return gamecube ? "R click" : "Misc 3";
        case SDL_GAMEPAD_BUTTON_MISC4: return gamecube ? "L click" : "Misc 4";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Paddle R1";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Paddle L1";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Paddle R2";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Paddle L2";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
        default: return "Button " + std::to_string(int(button));
        }
    }

    static std::string PadBindingLabel(const platform::GamepadBinding& binding, SDL_GamepadType type)
    {
        std::string text = PadInputLabel(binding[0], type);
        if (binding[1] != platform::kGamepadNone) text += " / " + PadInputLabel(binding[1], type);
        return text;
    }

    void WindowSizeControl()
    {
        // The sizes the graphics card offers for the chosen display (its
        // display modes, as games list them), up to its largest: while the
        // window keeps the picture's shape, those of that shape (a picture
        // filling the window, stretched or the wider view, fits any). A size
        // larger than the display's usable area opens as the largest window
        // that fits. Sizes are logical units like the game window's (points
        // on a Retina Mac). A size the game kept from a resized window stays
        // listed as the current one.
        const bool any_shape = PictureFills();
        const bool four_three = draft_.aspect == "4:3";
        const int shape_w = four_three ? 4 : 16, shape_h = four_three ? 3 : 9;
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (draft_.monitor > 0 && draft_.monitor <= int(display_ids_.size())) display = display_ids_[size_t(draft_.monitor - 1)];
        SDL_Rect usable{};
        const bool known = display && SDL_GetDisplayUsableBounds(display, &usable);
        auto fits = [&](int w, int h) {
            if (!known) return true;
            const auto fit = platform::FitWindowSize(w, h, usable.w, usable.h);
            return fit.width == w && fit.height == h;
        };
        auto offered = [&](int w, int h) {
            return w > 0 && h > 0 && (any_shape || std::abs(double(w) * shape_h / (double(h) * shape_w) - 1.0) < 0.01);
        };
        std::vector<std::array<int, 2>> sizes;
        auto offer = [&](int w, int h) {
            const std::array<int, 2> size{w, h};
            if (offered(w, h) && std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
        };
        int count = 0;
        if (SDL_DisplayMode** modes = display ? SDL_GetFullscreenDisplayModes(display, &count) : nullptr)
        {
            for (int i = 0; i < count; ++i) offer(modes[i]->w, modes[i]->h);
            SDL_free(modes);
        }
        // A display that reports no mode of the shape (a Wayland compositor
        // without scaling): the common sizes up to the display's own.
        if (sizes.empty())
        {
            static const std::array<int, 2> common[] = {{3840, 2160}, {2560, 1600}, {2560, 1440}, {1920, 1440},
                {1920, 1200}, {1920, 1080}, {1680, 1050}, {1600, 1200}, {1600, 900}, {1440, 1080}, {1440, 900},
                {1280, 800}, {1280, 720}, {1024, 768}, {800, 600}};
            SDL_Rect bounds{};
            const bool bounded = display && SDL_GetDisplayBounds(display, &bounds);
            for (const auto& size : common)
                if (!bounded || (size[0] <= bounds.w && size[1] <= bounds.h)) offer(size[0], size[1]);
        }
        std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
            return a[0] != b[0] ? a[0] > b[0] : a[1] > b[1];
        });
        auto label = [&](int w, int h) { return SizeLabel(w, h, any_shape); };
        const std::array<int, 2> current{draft_.width, draft_.height};
        const bool listed = std::find(sizes.begin(), sizes.end(), current) != sizes.end();
        const std::string preview = label(draft_.width, draft_.height) + (listed ? "" : "  (current)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##size", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (!listed) ImGui::Selectable(preview.c_str(), true);
            for (const auto& size : sizes)
                if (ImGui::Selectable(label(size[0], size[1]).c_str(), size == current))
                { draft_.width = size[0]; draft_.height = size[1]; dirty_ = true; }
            ImGui::EndCombo();
        }
        if (!fits(draft_.width, draft_.height))
        {
            const auto shrunk = platform::FitWindowSize(draft_.width, draft_.height, usable.w, usable.h);
            const std::string note = "More than a window can take on this display: the game opens it at "
                + std::to_string(shrunk.width) + " x " + std::to_string(shrunk.height) + ", the largest that fits.";
            TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
        }
        else if (!any_shape)
        {
            // The game gives the window the picture's shape: the same width.
            const auto shaped = SnapToShape(draft_.width, draft_.height, shape_w, shape_h);
            if (shaped.height != draft_.height)
            {
                const std::string note = "Not " + std::string(four_three ? "4:3" : "16:9") + ": the game opens it at "
                    + std::to_string(shaped.width) + " x " + std::to_string(shaped.height) + ", the picture's shape.";
                TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
            }
        }
    }

    // "1280 x 720", with its shape ("   (16:9)") when `with_shape`.
    static std::string SizeLabel(int w, int h, bool with_shape)
    {
        std::string text = std::to_string(w) + " x " + std::to_string(h);
        if (!with_shape || w <= 0 || h <= 0) return text;
        static const struct { double ratio; const char* name; } shapes[] = {{16.0 / 9, "16:9"},
            {16.0 / 10, "16:10"}, {4.0 / 3, "4:3"}, {5.0 / 4, "5:4"}, {3.0 / 2, "3:2"}, {32.0 / 9, "32:9"}};
        const double ratio = double(w) / h;
        for (const auto& shape : shapes)
            if (std::abs(ratio / shape.ratio - 1.0) < 0.01) return text + "   (" + shape.name + ")";
        // Ultrawide displays (2560 x 1080, 3440 x 1440, 3840 x 1600) are all sold as 21:9.
        return ratio > 2.3 && ratio < 2.42 ? text + "   (21:9)" : text;
    }

    // display.fullscreen_resolution: Desktop (the display's own resolution, no
    // mode change), or one of the display's other modes, largest first; the
    // picture keeps or fills it as the aspect ratio says.
    void FullscreenResolutionControl()
    {
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (draft_.monitor > 0 && draft_.monitor <= int(display_ids_.size())) display = display_ids_[size_t(draft_.monitor - 1)];
        const SDL_DisplayMode* desktop = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
        std::vector<std::array<int, 2>> sizes;
        int count = 0;
        if (SDL_DisplayMode** modes = display ? SDL_GetFullscreenDisplayModes(display, &count) : nullptr)
        {
            for (int i = 0; i < count; ++i)
            {
                const std::array<int, 2> size{modes[i]->w, modes[i]->h};
                if (size[0] > 0 && size[1] > 0 && !(desktop && size[0] == desktop->w && size[1] == desktop->h)
                    && std::find(sizes.begin(), sizes.end(), size) == sizes.end())
                    sizes.push_back(size);
            }
            SDL_free(modes);
        }
        std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
            return a[0] != b[0] ? a[0] > b[0] : a[1] > b[1];
        });
        const std::string desktop_label = "Desktop" + (desktop ? "   (" + SizeLabel(desktop->w, desktop->h, false) + ")"
                                                               : std::string());
        const auto chosen = ParseResolution(draft_.fullscreen_resolution);
        const std::array<int, 2> current{chosen.width, chosen.height};
        const bool listed = chosen.width == 0 || std::find(sizes.begin(), sizes.end(), current) != sizes.end();
        const std::string preview = chosen.width == 0 ? desktop_label
            : SizeLabel(chosen.width, chosen.height, true) + (listed ? "" : "  (not on this display)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##fullscreen_resolution", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable(desktop_label.c_str(), chosen.width == 0))
            { draft_.fullscreen_resolution = std::string(kDesktopResolution); dirty_ = true; }
            if (!listed) ImGui::Selectable(preview.c_str(), true);
            for (const auto& size : sizes)
                if (ImGui::Selectable(SizeLabel(size[0], size[1], true).c_str(), size == current))
                { draft_.fullscreen_resolution = FormatResolution(size[0], size[1]); dirty_ = true; }
            ImGui::EndCombo();
        }
        if (!listed)
            TextWrappedColored(CurrentFonts().caption, color::warning,
                               "This display has no such mode, so fullscreen keeps the display's own resolution.");
        else if (chosen.width && desktop && desktop->w > 0 && desktop->h > 0
                 && std::abs(double(chosen.width) * desktop->h / (double(chosen.height) * desktop->w) - 1.0) > 0.01)
        {
            const std::string note = "Another shape than this " + SizeLabel(desktop->w, desktop->h, false)
                + " display: some displays stretch it; others show bars.";
            TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
        }
    }

    void DisplayChoice()
    {
        const float width = ImGui::GetContentRegionAvail().x;
        std::string preview = "Default display";
        if (draft_.monitor > 0)
            preview = draft_.monitor <= int(displays_.size()) ? displays_[size_t(draft_.monitor - 1)]
                                                               : "Display " + std::to_string(draft_.monitor) + " (not connected)";
        ImGui::SetNextItemWidth(width);
        ImGui::BeginDisabled(displays_.size() <= 1 && draft_.monitor == 0);
        if (ImGui::BeginCombo("##monitor", preview.c_str()))
        {
            if (ImGui::Selectable("Default display", draft_.monitor == 0)) { draft_.monitor = 0; dirty_ = true; }
            for (size_t i = 0; i < displays_.size(); ++i)
                if (ImGui::Selectable(displays_[i].c_str(), draft_.monitor == int(i) + 1))
                { draft_.monitor = int(i) + 1; dirty_ = true; }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    void AudioPage()
    {
        PageHeader("Audio", "The game's volume and the level of each kind of sound. The music, sound effect and "
                            "voice settings in the game's own Options menu still apply.");
        BeginCard("##output", "Output", "Plays through your system's default audio device.", Icon::Audio);
        if (BeginRows("##audio_rows"))
        {
            Row("Master volume", "Scales the game's speaker output, like the volume of a TV.");
            ImGui::BeginDisabled(draft_.mute);
            if (PercentSlider("##volume", &draft_.master_volume, ImGui::GetContentRegionAvail().x)) dirty_ = true;
            ImGui::EndDisabled();
            Row("Mute", "Silences the game without changing the volume.");
            if (Toggle("##mute", &draft_.mute)) dirty_ = true;
            EndRows();
        }
        EndCard();

        // [audio] <group>_volume: applied by the game module when the game starts.
        BeginCard("##sound_levels", "Sound levels",
                  "Each kind of sound as a share of the level the game gives it, on the PC only. Applies when the game starts.",
                  Icon::Sliders);
        if (BeginRows("##sound_level_rows"))
        {
            ImGui::BeginDisabled(draft_.mute);
            for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
            {
                Row(kSoundGroups[n].name, kSoundGroups[n].description);
                ImGui::PushID(kSoundGroups[n].key);
                if (PercentSlider("##level", &draft_.sound_volumes[n], ImGui::GetContentRegionAvail().x)) dirty_ = true;
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            Row("Reset levels", "Every kind of sound back to 100%, the game's own mix.");
            const auto defaults = DefaultSoundVolumes();
            if (SecondaryButton("sound_levels_reset", "Reset levels", {ImGui::GetContentRegionAvail().x, Dp(40)},
                                draft_.sound_volumes != defaults, Icon::Refresh))
            {
                draft_.sound_volumes = defaults;
                dirty_ = true;
            }
            EndRows();
        }
        EndCard();
    }

    // Keycaps side by side. `together`: the keys are pressed at once
    // ("Alt + Enter"), not alternatives.
    static void KeyCaps(std::initializer_list<const char*> keys, bool together)
    {
        bool first = true;
        for (const char* key : keys)
        {
            if (!first) ImGui::SameLine(0, Dp(6));
            if (!first && together)
            {
                // Centred on the 28 dp keycaps.
                const float height = TextSize(CurrentFonts().label, "+").y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Dp(28) - height) * 0.5f);
                TextColored(CurrentFonts().label, color::muted, "+");
                ImGui::SameLine(0, Dp(6));
            }
            KeyCap(key);
            first = false;
        }
    }

    void KeyRow(const char* button, std::initializer_list<const char*> keys, const char* alternative = nullptr,
                const char* note = nullptr, bool together = false)
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, Dp(40));
        ImGui::TableSetColumnIndex(0);
        TextColored(CurrentFonts().label, color::text, button);
        if (note)
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
            TextColored(CurrentFonts().caption, color::muted, note);
        }
        ImGui::TableSetColumnIndex(1);
        KeyCaps(keys, together);
        if (alternative)
        {
            ImGui::SameLine(0, Dp(10));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(4));
            TextColored(CurrentFonts().caption, color::muted, alternative);
        }
    }

    void PollWiimotes()
    {
        const Uint64 now = SDL_GetTicksNS();
        if (wii_probe_.Active())
        {
            if (wii_probe_.Poll())
            {
                AcceptWiimoteProbe(wii_probe_.Result());
                wii_known_ = true;
                wii_next_probe_ns_ = now + 2000000000ull;
            }
        }
        else if (now >= wii_next_probe_ns_)
            wii_probe_.Start(250);
    }

    // A remote that is still listed but missed one status answer keeps its
    // last state, so a flaky Bluetooth link does not flip the Players card.
    // It is gone once its device disappears or it misses twice in a row.
    void AcceptWiimoteProbe(platform::WiimoteHidStatus status)
    {
        for (const auto& previous : wii_status_.remotes)
        {
            const bool answered = std::any_of(status.remotes.begin(), status.remotes.end(),
                                              [&](const auto& remote) { return remote.id == previous.id; });
            const bool silent = std::find(status.silent.begin(), status.silent.end(), previous.id) != status.silent.end();
            if (!answered && silent && wii_held_.insert(previous.id).second) status.remotes.push_back(previous);
            else wii_held_.erase(previous.id);
        }
        platform::SortWiimoteRemotes(status.remotes);
        wii_status_ = std::move(status);
        UpdateRemoteLights();
    }

    // Each connected Wii Remote shows on its lights which player it is if the
    // game starts now, lights off when it does not play, as the game does; it
    // rumbles briefly when it becomes a player (with rumble on).
    void UpdateRemoteLights()
    {
        const auto lineup = StartLineup();
        std::map<std::string, int> shown;
        for (const auto& remote : wii_status_.remotes)
        {
            const auto it = std::find(lineup.playing.begin(), lineup.playing.end(), remote.id);
            const int player = it == lineup.playing.end() ? -1 : int(it - lineup.playing.begin());
            const auto before = remote_players_.find(remote.id);
            if (before == remote_players_.end() || before->second != player)
            {
                if (!remote_lights_) remote_lights_ = std::make_unique<platform::WiimotePlayerLights>();
                remote_lights_->Show(remote.path, player, player >= 0 && draft_.rumble);
            }
            shown[remote.id] = player;
        }
        remote_players_ = std::move(shown);
    }

    bool RemotePlayer() const
    {
        return std::any_of(draft_.players.begin(), draft_.players.end(),
                           [](const std::string& device) { return platform::IsWiimoteId(device); });
    }

    // Who plays if the game starts now (player_lineup.h). Wii Remotes count as
    // connected until a check says otherwise.
    PlayerLineup StartLineup() const
    {
        return PredictLineup(
            draft_.players, [this](const std::string& id) { return !wii_known_ || ConnectedRemote(id) != nullptr; },
            int(gamepad_names_.size()));
    }

    // The connected remote with this id, or null.
    const platform::WiimoteHidRemote* ConnectedRemote(const std::string& id) const
    {
        for (const auto& remote : wii_status_.remotes)
            if (remote.id == id) return &remote;
        return nullptr;
    }

    // "Wii Remote (DolphinBar slot 2)" or "Wii Remote Plus (Bluetooth C6:90:76)".
    std::string RemoteName(const std::string& id) const
    {
        const auto* remote = ConnectedRemote(id);
        const std::string model = remote && remote->plus ? "Wii Remote Plus" : "Wii Remote";
        const int slot = platform::WiimoteDolphinBarSlot(id);
        if (slot >= 0) return model + " (DolphinBar slot " + std::to_string(slot + 1) + ")";
        const auto address = platform::WiimoteBluetoothAddress(id);
        return model + " (Bluetooth " + (address.size() > 8 ? address.substr(address.size() - 8) : address) + ")";
    }

    // Windows: connect and pair Wii Remotes over Bluetooth (wiimote_bluetooth.h).
    void BluetoothRows()
    {
        const auto search = bluetooth_search_.Status();
        if (!search.supported || !BeginRows("##bluetooth_rows")) return;
        Row("Continuous scanning", !search.adapter ? "No Bluetooth adapter found."
                                   : draft_.bluetooth_search
                                       ? "Press 1+2 on a Wii Remote to connect it, here and when a player's Remote "
                                         "drops out of a game."
                                       : "Off: after 1+2 press Refresh, or pair the Remote with Sync.");
        if (Toggle("##bluetooth_search", &draft_.bluetooth_search)) dirty_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Searching can briefly disturb other Bluetooth devices, such as headphones.\n"
                              "In a game it searches only while a player's Wii Remote is missing.");
        std::string text = search.syncing ? "Syncing: press the red SYNC button under the Remote's battery cover now."
                           : search.resetting ? "Forgetting the paired Wii Remotes..."
                           : "Sync pairs a Remote for good: press Sync, then the red SYNC button under its battery "
                             "cover. It then connects with any button.";
        if (!search.syncing && !search.resetting && !search.last.empty()) text += " Last: " + search.last + ".";
        Row("Bluetooth pairing", text.c_str());
        const bool idle = !search.syncing && !search.resetting;
        const float gap = Dp(8), width = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3;
        if (SecondaryButton("##bluetooth_refresh", "Refresh", {width, Dp(36)}, idle)) bluetooth_search_.Refresh();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search once (4 s): press 1+2 on the Remote first.");
        ImGui::SameLine(0, gap);
        if (SecondaryButton("##bluetooth_sync", search.syncing ? "Syncing..." : "Sync", {width, Dp(36)}, idle))
            bluetooth_search_.Sync();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pair Wii Remotes for good (about 12 s), as on a Wii.\n"
                              "A paired Remote connects with any button, also in a game.");
        ImGui::SameLine(0, gap);
        if (SecondaryButton("##bluetooth_reset", "Reset", {width, Dp(36)}, idle)) bluetooth_search_.Reset();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Forget every Wii Remote Windows remembers, for one that no longer connects\n"
                              "(for example after it was synced with a Wii again). Sync it again afterwards.");
        EndRows();
    }

    // controls.player1-4: keyboard & mouse, a Wii Remote (in a DolphinBar
    // slot or paired over Bluetooth), a gamepad or off. Players fill in order
    // and each device plays once. Controllers missing when the game starts
    // drop out; with nobody left keyboard & mouse is player 1, alone.
    void PlayersCard()
    {
        const auto& status = wii_status_;
        const bool bar = status.dolphinbar;
        int bluetooth = 0;
        for (const auto& remote : status.remotes) bluetooth += remote.slot < 0;
        auto& players = draft_.players;
        const auto lineup = StartLineup();
        const auto note = MissingControllersNote(players, lineup);
        // The layout stays put while remotes come and go: a fixed subtitle and
        // one line of status chips, whatever the remotes do.
        BeginCard("##players", "Players",
                  "Players fill in order and each controller plays once. Changes apply when the game starts.",
                  Icon::Gamepad);
        bool chip = false;
        const auto next_chip = [&] { if (chip) ImGui::SameLine(0, Dp(8)); chip = true; };
        if (bar)
        {
            next_chip();
            Chip("DolphinBar", color::accent, Icon::Check);
        }
        else if (status.dolphinbar_other_mode)
        {
            next_chip();
            Chip("DolphinBar: switch to mode 4", color::warning, Icon::Warning);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The DolphinBar is in a mouse or gamepad mode. Press its MODE button until light 4 is on.");
        }
        if (bluetooth)
        {
            next_chip();
            const auto text = "Bluetooth: " + std::to_string(bluetooth) + (bluetooth == 1 ? " Wii Remote" : " Wii Remotes");
            Chip(text.c_str(), color::accent, Icon::Check);
        }
        if (status.no_permission)
        {
            next_chip();
            Chip("Wii Remote without access", color::warning, Icon::Warning);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A Wii Remote is connected, but this user can't open it.");
        }
        if (!chip)
        {
            Chip(wii_known_ ? "No Wii Remote" : "Looking for Wii Remotes...", color::muted,
                 wii_known_ ? Icon::Info : Icon::None);
            if (wii_known_)
            {
                ImGui::SameLine(0, Dp(10));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(5));
                TextColored(CurrentFonts().caption, color::muted,
                            "Pair one over Bluetooth, or use a Mayflash DolphinBar in mode 4.");
            }
        }

        const auto name = [&](const std::string& device) {
            if (device == "keyboard") return std::string("Keyboard & mouse");
            if (device == "off") return std::string("Off");
            if (device.rfind("gamepad", 0) == 0)
            {
                const int slot = device.back() - '1';
                return slot < int(gamepad_names_.size()) ? "Gamepad " + std::to_string(slot + 1) + ": " + gamepad_names_[slot]
                                                         : "Gamepad " + std::to_string(slot + 1) + "  (not connected)";
            }
            const auto* remote = ConnectedRemote(device);
            return RemoteName(device) + (!remote ? "  (not connected)" : remote->nunchuk ? " + Nunchuk" : "");
        };
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 5));
        if (ImGui::BeginTable("##player_slots", 4))
        {
            ImGui::TableSetupColumn("player", ImGuiTableColumnFlags_WidthFixed, Dp(110));
            ImGui::TableSetupColumn("device", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, Dp(84));
            ImGui::TableSetupColumn("battery", ImGuiTableColumnFlags_WidthFixed, Dp(84));
            for (int n = 0; n < 4; ++n)
            {
                ImGui::PushID(n);
                ImGui::TableNextRow(ImGuiTableRowFlags_None, Dp(36));
                ImGui::TableSetColumnIndex(0);
                const std::string shown = players[n];
                const bool enabled = n == 0 || players[n - 1] != "off";
                ImGui::AlignTextToFramePadding();
                TextColored(CurrentFonts().label, enabled || n == 0 ? color::text : color::dim,
                            ("Player " + std::to_string(n + 1)).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::BeginDisabled(!enabled);
                // A gamepad also shows its profile beside it.
                const bool gamepad_row = shown.rfind("gamepad", 0) == 0;
                const float profile_width = Dp(150);
                ImGui::SetNextItemWidth(gamepad_row ? -(profile_width + Dp(20)) : -Dp(12));
                if (ImGui::BeginCombo("##device", name(shown).c_str()))
                {
                    const auto option = [&](const std::string& device) {
                        int other = -1;
                        for (int m = 0; m < 4; ++m)
                            if (m != n && device != "off" && players[m] == device) other = m;
                        // Choosing another player's device swaps the two; an
                        // empty player cannot take one (that would leave a gap).
                        if (other >= 0 && players[n] == "off") return;
                        const auto label = name(device)
                            + (other >= 0 ? "  (swap with player " + std::to_string(other + 1) + ")" : "");
                        if (ImGui::Selectable(label.c_str(), players[n] == device))
                        {
                            if (other >= 0) players[other] = players[n];
                            players[n] = device;
                            if (device == "off")
                                for (int m = n + 1; m < 4; ++m) players[m] = "off";
                            dirty_ = true;
                        }
                    };
                    option("keyboard");
                    // Connected Wii Remotes, then this player's remote when it is away.
                    for (const auto& remote : status.remotes) option(remote.id);
                    if (platform::IsWiimoteId(players[n]) && !ConnectedRemote(players[n])) option(players[n]);
                    for (int pad = 0; pad < 4; ++pad)
                    {
                        const auto device = "gamepad" + std::to_string(pad + 1);
                        if (pad < int(gamepad_names_.size()) || players[n] == device) option(device);
                    }
                    if (n > 0) option("off");
                    ImGui::EndCombo();
                }
                if (gamepad_row)
                {
                    ImGui::SameLine(0, Dp(8));
                    ProfileCombo("##profile", std::size_t(shown.back() - '1'), profile_width);
                }
                ImGui::EndDisabled();
                const auto* remote = platform::IsWiimoteId(shown) ? ConnectedRemote(shown) : nullptr;
                ImGui::TableSetColumnIndex(3);
                if (remote) BatteryIcon(remote->battery);
                else if (lineup.missing[std::size_t(n)])
                {
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    DrawIcon(ImGui::GetWindowDrawList(), Icon::Warning, {pos.x + Dp(9), pos.y + Dp(12.5f)}, Dp(17),
                             Col(color::warning));
                    ImGui::Dummy(Dp(25, 25));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not connected. Connect it before you start the game.");
                }
                ImGui::TableSetColumnIndex(2);
                if (shown.rfind("gamepad", 0) == 0)
                {
                    const std::size_t pad = std::size_t(shown.back() - '1');
                    if (SecondaryButton("##layout", "", Dp(36, 30), true, Icon::Sliders)) OpenGamepadView(pad);
                    if (ImGui::IsItemHovered()) GamepadLayoutTooltip(pad);
                }
                if (shown == "keyboard")
                {
                    if (SecondaryButton("##keys", "", Dp(36, 30), true, Icon::Sliders)) OpenKeyboardView();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keyboard & mouse keys");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        // Under the table a block says what happens to a controller that is
        // not connected: the rule, or in warning colour what happens now. It
        // is as tall as the longest of its texts (every such controller
        // missing included), so the rows below stay put as controllers come
        // and go.
        if (std::any_of(players.begin(), players.end(),
                        [](const std::string& device) { return device != "keyboard" && device != "off"; }))
        {
            const std::string rule = "Controllers that are not connected when the game starts drop out. With nobody "
                                     "left, keyboard & mouse plays as player 1.";
            const auto& fonts = CurrentFonts();
            const float width = ImGui::GetContentRegionAvail().x;
            const auto height = [&](const std::string& text) {
                return fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, width, text.c_str()).y;
            };
            const float block = std::max(
                {height(rule), height(MissingControllersNote(players, WorstLineup(players))), height(note)});
            ImGui::GetWindowDrawList()->AddText(fonts.caption, FontSize(fonts.caption), ImGui::GetCursorScreenPos(),
                                                Col(note.empty() ? color::muted : color::warning),
                                                (note.empty() ? rule : note).c_str(), nullptr, width);
            ImGui::Dummy({width, block});
        }
        // Single player on a Wii Remote or gamepad only.
        if (players[0] != "keyboard" && players[1] == "off" && BeginRows("##mouse_rows"))
        {
            Row("Mouse pointer", "The mouse also points for player 1: left click is A, right click is B. "
                "The controller keeps working. Only when player 1 plays alone.");
            if (Toggle("##mouse_pointer", &draft_.mouse_pointer)) dirty_ = true;
            EndRows();
        }
        BluetoothRows();
        if (status.no_permission && BeginRows("##wiimote_access_rows"))
        {
#if defined(__linux__)
            Row("Wii Remote access", "A Wii Remote is connected, but this user can't open it. Install the udev rule "
                "(copy the commands, run them in a terminal), then turn the remote off and on.");
            if (SecondaryButton("##copy_udev", "Copy commands", {ImGui::GetContentRegionAvail().x, Dp(36)}, true, Icon::Copy))
            {
                if (SDL_SetClipboardText(kWiimoteUdevCommands))
                    Notice("Udev rule commands copied to the clipboard.", NoticeKind::Success);
            }
#else
            Row("Wii Remote access", "A Wii Remote is connected, but it could not be opened. Close other programs "
                "that use it (Dolphin, Steam), then turn the remote off and on.");
            ImGui::Dummy({0, 0});
#endif
            EndRows();
        }
        EndCard();
    }

    bool GamepadPlayer() const
    {
        return std::any_of(draft_.players.begin(), draft_.players.end(),
                           [](const std::string& device) { return device.rfind("gamepad", 0) == 0; });
    }

    // The Wii's Sensor Bar and Wii Remote settings. Shown from the settings,
    // not from what is connected right now: the card stays while remotes come
    // and go, and while a setting differs from the Wii's default.
    void WiiRemoteCard()
    {
        const Settings defaults;
        if (!RemotePlayer() && draft_.sensor_bar == defaults.sensor_bar &&
            draft_.ir_sensitivity == defaults.ir_sensitivity && draft_.speaker_volume == defaults.speaker_volume)
            return;
        BeginCard("##wii_remote", "Wii Remote", "As in the Wii's own settings. Changes apply when the game starts.",
                  Icon::Target);
        if (BeginRows("##wii_remote_rows"))
        {
            Row("Sensor bar", "Where your sensor bar or DolphinBar sits. The game aims the Wii Remote pointer from it.");
            int position = draft_.sensor_bar == "top" ? 0 : 1;
            if (Segmented("##sensor_bar", &position, {"Above the screen", "Below the screen"},
                          ImGui::GetContentRegionAvail().x))
            {
                draft_.sensor_bar = position == 0 ? "top" : "bottom";
                dirty_ = true;
            }
            Row("Sensitivity", "How readily the Remote's camera sees the sensor bar (3 is the Wii's default). Raise it "
                "when the pointer drops out far from the bar, lower it when lamps or windows disturb it.");
            int level = std::clamp(draft_.ir_sensitivity, 1, 5) - 1;
            if (Segmented("##ir_sensitivity", &level, {"1", "2", "3", "4", "5"}, ImGui::GetContentRegionAvail().x))
            {
                draft_.ir_sensitivity = level + 1;
                dirty_ = true;
            }
            Row("Speaker volume", "Sounds the game plays on each player's Wii Remote; they play on the TV as well. "
                "Off sends no sound to the Remotes.");
            if (PercentSlider("##speaker_volume", &draft_.speaker_volume, ImGui::GetContentRegionAvail().x, "Off"))
                dirty_ = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Speaker sound depends on the Bluetooth link: if it crackles, turn it down or off.");
            EndRows();
        }
        EndCard();
    }

    // controls.rumble for every controller, controls.rumble_strength for
    // gamepads (a Wii Remote's motor is on or off).
    void RumbleCard()
    {
        const Settings defaults;
        const bool controller = std::any_of(draft_.players.begin(), draft_.players.end(),
            [](const std::string& device) { return device != "keyboard" && device != "off"; });
        if (!controller && draft_.rumble == defaults.rumble && draft_.rumble_strength == defaults.rumble_strength)
            return;
        BeginCard("##rumble", "Rumble", nullptr, Icon::Gamepad);
        if (BeginRows("##rumble_rows"))
        {
            Row("Rumble", "The game's rumble on Wii Remotes and gamepads, like the Wii's rumble setting.");
            if (Toggle("##rumble", &draft_.rumble)) dirty_ = true;
            if (GamepadPlayer() || draft_.rumble_strength != defaults.rumble_strength)
            {
                Row("Gamepad strength", "How strongly gamepads rumble. A Wii Remote's motor is either on or off.");
                ImGui::BeginDisabled(!draft_.rumble);
                if (PercentSlider("##rumble_strength", &draft_.rumble_strength, ImGui::GetContentRegionAvail().x, "Off"))
                    dirty_ = true;
                ImGui::EndDisabled();
            }
            EndRows();
        }
        EndCard();
    }

    // Controls: the Players view, or the keyboard & mouse keys or gamepad
    // buttons view that slides in from a player row and saves when it goes back.
    void ControlsPage()
    {
        const bool detail = keyboard_view_ || gamepad_view_;
        const float t = Animate(ImGui::GetID("##keyboard_slide"), detail, 14.0f);
        const float shift = detail ? (1.0f - t) * Dp(120) : -t * Dp(120);
        if (std::fabs(shift) > 0.5f) ImGui::Indent(shift);
        if (keyboard_view_) KeyboardView();
        else if (gamepad_view_) GamepadView();
        else ControlsMain();
        if (std::fabs(shift) > 0.5f) ImGui::Unindent(shift);
    }

    void OpenKeyboardView()
    {
        keyboard_view_ = true;
        capture_action_ = capture_slot_ = hotkey_capture_ = -1;
    }

    void CloseKeyboardView()
    {
        keyboard_view_ = false;
        capture_action_ = capture_slot_ = -1;
        if (dirty_ && config_ok_) Save();
    }

    platform::Hotkeys DraftHotkeys() const { return platform::ParseHotkeys(draft_.hotkeys); }
    // The hotkeys' default keys; one stays unused while a keyboard & mouse
    // action has its default key, as in a settings file from before it.
    std::array<std::string, HotkeyCount> DefaultHotkeyNames() const
    {
        auto names = Settings::DefaultHotkeys();
        for (std::size_t n = 0; n < HotkeyCount; ++n)
        {
            const auto hotkey = platform::ParseHotkey(kHotkeys[n].defaults);
            if (hotkey.modifiers != SDL_KMOD_NONE) continue;
            for (const auto& text : draft_.keys)
            {
                const auto binding = platform::ParseKeyBinding(text);
                if (binding[0] == hotkey.key || binding[1] == hotkey.key) names[n] = kNoHotkey;
            }
        }
        return names;
    }
    platform::KeyBindings DraftBindings() const { return platform::ParseKeyBindings(draft_.keys, DraftHotkeys()); }

    // A key or mouse button pressed while a keycap waits: it takes that slot,
    // moving it from another action unless it is that action's only key.
    void CaptureKey(platform::KeyInput code)
    {
        const int action = capture_action_, slot = capture_slot_;
        capture_action_ = capture_slot_ = -1;
        if (action < 0) return;
        const std::string name = platform::KeyInputName(code);
        if (name.empty()) return;
        if (const auto hotkey = DraftHotkeys().Find(code); hotkey != HotkeyCount)
        {
            Notice(name + " is the hotkey " + kHotkeys[hotkey].label
                       + " (Hotkeys on the Controls page) and cannot be assigned.",
                   NoticeKind::Warning);
            return;
        }
        auto bindings = DraftBindings();
        auto& mine = bindings[action];
        if (mine[0] == code || mine[1] == code) return;
        int moved = -1;
        for (std::size_t other = 0; other < KeyActionCount; ++other)
        {
            if (int(other) == action) continue;
            auto& binding = bindings[other];
            for (int k = 0; k < 2; ++k)
            {
                if (binding[k] != code) continue;
                if (binding[1 - k] == platform::kKeyNone)
                {
                    Notice(name + " is the only key of " + kKeyActions[other].label
                               + ". Give that action another key first.", NoticeKind::Warning);
                    return;
                }
                binding = {binding[1 - k], platform::kKeyNone};
                moved = int(other);
            }
        }
        mine[slot] = code;
        if (mine[0] == platform::kKeyNone) mine = {mine[1], platform::kKeyNone};
        draft_.keys[action] = platform::FormatKeyBinding(mine);
        if (moved >= 0) draft_.keys[moved] = platform::FormatKeyBinding(bindings[moved]);
        dirty_ = true;
        Notice(name + " now presses " + kKeyActions[action].label
                   + (moved >= 0 ? std::string(" (moved from ") + kKeyActions[moved].label + ")" : std::string()) + ".",
               NoticeKind::Info);
    }

    // A key pressed while a hotkey's keycaps wait (hotkey_capture_: its
    // HotkeyIndex), with the Ctrl, Shift and Alt held (`mod`): Escape stops
    // waiting, Backspace or Delete leaves the hotkey unused, a modifier waits
    // for the key it goes with. The same keys as another hotkey, the Windows
    // or Command key, or a keyboard & mouse key pressed alone are refused.
    void CaptureHotkey(SDL_Scancode code, SDL_Keymod mod)
    {
        const int which = hotkey_capture_;
        if (which < 0 || which >= int(HotkeyCount)) return;
        if (code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI) return;
        hotkey_capture_ = -1;
        const char* label = kHotkeys[std::size_t(which)].label;
        const platform::Hotkey hotkey{code, platform::HotkeyModifiers(mod)};
        if (hotkey.modifiers == SDL_KMOD_NONE && !(mod & SDL_KMOD_GUI))
        {
            if (code == SDL_SCANCODE_ESCAPE) return;
            if (code == SDL_SCANCODE_BACKSPACE || code == SDL_SCANCODE_DELETE)
            {
                if (draft_.hotkeys[std::size_t(which)] == kNoHotkey) return;
                draft_.hotkeys[std::size_t(which)] = kNoHotkey;
                dirty_ = true;
                Notice(std::string(label) + " has no hotkey now.", NoticeKind::Info);
                return;
            }
        }
        if (mod & SDL_KMOD_GUI)
        {
            Notice("Combinations with the Windows or Command key stay with the system.", NoticeKind::Warning);
            return;
        }
        const std::string name = platform::HotkeyLabel(hotkey);
        if (!platform::HotkeyAllowed(hotkey) || name.empty()) return;
        const auto hotkeys = DraftHotkeys();
        for (std::size_t other = 0; other < HotkeyCount; ++other)
            if (hotkeys.keys[other] == hotkey)
            {
                if (int(other) != which)
                    Notice(name + " is already the hotkey " + kHotkeys[other].label + ".", NoticeKind::Warning);
                return;
            }
        // A key pressed alone would also press its keyboard & mouse action.
        if (hotkey.modifiers == SDL_KMOD_NONE)
        {
            const auto bindings = DraftBindings();
            for (std::size_t action = 0; action < KeyActionCount; ++action)
                if (bindings[action][0] == code || bindings[action][1] == code)
                {
                    Notice(name + " presses " + kKeyActions[action].label + " for keyboard & mouse. Give that action "
                               "another key first, or hold Ctrl, Shift or Alt with it.",
                           NoticeKind::Warning);
                    return;
                }
        }
        draft_.hotkeys[std::size_t(which)] = platform::HotkeyName(hotkey);
        dirty_ = true;
        Notice(name + " is now the hotkey " + label + ".", NoticeKind::Info);
    }

    // [hotkeys]: keys of the game window, whoever plays, each a key alone or
    // with Ctrl, Shift or Alt. No HUD sits with its key.
    void HotkeysCard()
    {
        BeginCard("##hotkeys", "Hotkeys", "Keys of the game window, whoever plays. Click a hotkey, then press the new "
                  "key, alone or with Ctrl, Shift or Alt. Escape cancels; Backspace or Delete leaves it unused.",
                  Icon::Keyboard);
        const auto hotkeys = DraftHotkeys();
        bool clicked = false;
        const auto hotkey = [&](HotkeyIndex which) {
            Row(kHotkeys[which].label, kHotkeys[which].note);
            const bool waiting = hotkey_capture_ == int(which);
            std::vector<std::string> keys;
            if (waiting)
            {
                // The modifiers held so far, then the key still to come.
                const SDL_Keymod held = platform::HotkeyModifiers(SDL_GetModState());
                keys = platform::HotkeyKeys({SDL_SCANCODE_A, held});
                keys.back() = keys.size() > 1 ? "..." : "Press keys...";
            }
            else if (hotkeys[which].key) keys = platform::HotkeyKeys(hotkeys[which]);
            else keys = {"Not set"};
            ImGui::PushID(int(which));
            if (KeyComboButton("##hotkey", keys, waiting))
            {
                clicked = true;
                hotkey_capture_ = waiting ? -1 : int(which);
            }
            if (ImGui::IsItemHovered() && !waiting) ImGui::SetTooltip("Click, then press the new keys");
            ImGui::PopID();
        };
        if (BeginRows("##hotkey_rows"))
        {
            hotkey(HotkeyScreenshot);
            hotkey(HotkeyFullscreen);
            hotkey(HotkeyStretch);
            hotkey(HotkeyResolutionUp);
            hotkey(HotkeyResolutionDown);
            hotkey(HotkeyHud);
            const std::string key = hotkeys[HotkeyHud].key ? platform::HotkeyLabel(hotkeys[HotkeyHud])
                                                           : std::string("its hotkey");
            const std::string note = "The scoreboard, player markers and captions. Off starts matches without them, "
                "for clean recordings; " + key + " shows them. The Mega Strike meter and its defence always stay.";
            Row("Show the HUD when a match starts", note.c_str());
            if (Toggle("##show_hud", &draft_.show_hud)) dirty_ = true;
            EndRows();
        }
        if (SecondaryButton("##reset_hotkeys", "Reset keys", Dp(140, 36), draft_.hotkeys != DefaultHotkeyNames(),
                            Icon::Refresh))
        {
            draft_.hotkeys = DefaultHotkeyNames();
            hotkey_capture_ = -1;
            dirty_ = true;
        }
        EndCard();
        // A click anywhere else stops waiting for keys.
        if (hotkey_capture_ >= 0 && !clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) hotkey_capture_ = -1;
    }

    void KeyboardView()
    {
        if (SecondaryButton("##back", "Players", Dp(124, 36), true, Icon::Back)) { CloseKeyboardView(); return; }
        ImGui::Dummy(Dp(0, 6));
        PageHeader("Keyboard & mouse", "Click a key, then press the new key or a middle or side mouse button "
                   "(Mouse 4, Mouse 5). Each action takes up to two. Changes are saved when you go back.");
        const auto bindings = DraftBindings();
        bool clicked = false;
        const auto rows = [&](std::size_t first, std::size_t last) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
            if (ImGui::BeginTable("##keys", 2))
            {
                ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch);
                for (std::size_t n = first; n < last; ++n)
                {
                    ImGui::PushID(int(n));
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, Dp(40));
                    ImGui::TableSetColumnIndex(0);
                    TextColored(CurrentFonts().label, color::text, kKeyActions[n].label);
                    if (kKeyActions[n].note)
                    {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
                        TextColored(CurrentFonts().caption, color::muted, kKeyActions[n].note);
                    }
                    ImGui::TableSetColumnIndex(1);
                    for (int slot = 0; slot < 2; ++slot)
                    {
                        const platform::KeyInput code = bindings[n][slot];
                        if (slot == 1 && code == platform::kKeyNone && bindings[n][0] == platform::kKeyNone) break;
                        const bool waiting = capture_action_ == int(n) && capture_slot_ == slot;
                        const std::string label = waiting ? "Press a key..." : code != platform::kKeyNone
                            ? platform::KeyInputName(code) : "+";
                        if (slot) ImGui::SameLine(0, Dp(6));
                        ImGui::PushID(slot);
                        if (KeyCapButton("##key", label.c_str(), waiting))
                        {
                            clicked = true;
                            capture_action_ = waiting ? -1 : int(n);
                            capture_slot_ = waiting ? -1 : slot;
                        }
                        if (ImGui::IsItemHovered() && !waiting)
                            ImGui::SetTooltip(code != platform::kKeyNone ? "Click, then press the new key or mouse button"
                                                                         : "Add a second key or mouse button");
                        ImGui::PopID();
                    }
                    if (bindings[n][1] != platform::kKeyNone)
                    {
                        ImGui::SameLine(0, Dp(6));
                        if (SecondaryButton("##remove", "", Dp(30, 30), true, Icon::Error))
                        {
                            draft_.keys[n] = platform::FormatKeyBinding({bindings[n][0], platform::kKeyNone});
                            dirty_ = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the second key");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        };
        BeginCard("##remote_keys", "Wii Remote", "Buttons of the keyboard's Wii Remote.", Icon::Keyboard);
        rows(KeyActionA, KeyActionStickUp);
        EndCard();
        BeginCard("##nunchuk_keys", "Nunchuk & motion", "The Nunchuk stick and buttons, and the shakes.", Icon::Gamepad);
        rows(KeyActionStickUp, KeyActionCount);
        EndCard();
        BeginCard("##mouse_keys", "Mouse",
                  "The screenshot, HUD and fullscreen keys are under Hotkeys on the Controls page.", Icon::Mouse);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
        if (ImGui::BeginTable("##fixed_keys", 2))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch);
            KeyRow("Pointer", {"Mouse"}, "left click is A, right click is B", "Point at menu items");
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        if (SecondaryButton("##reset_keys", "Reset to defaults", Dp(180, 36), draft_.keys != Settings::DefaultKeys(),
                            Icon::Refresh))
        {
            draft_.keys = Settings::DefaultKeys();
            capture_action_ = capture_slot_ = -1;
            dirty_ = true;
        }
        EndCard();
        // A click anywhere else stops waiting for a key.
        if (capture_action_ >= 0 && !clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            capture_action_ = capture_slot_ = -1;
    }

    // Gamepad profiles: the built-in Default and named button layouts kept in
    // recharged.ini, each made for one kind of controller; each gamepad plays
    // with the one chosen for it.
    void OpenGamepadView(std::size_t slot)
    {
        gamepad_view_ = true;
        pad_slot_ = slot;
        edit_profile_ = draft_.PadProfileIndex(slot);
        renaming_ = confirm_delete_ = false;
        pad_capture_action_ = pad_capture_slot_ = -1;
    }

    // Going back assigns the profile on screen to the gamepad the page was opened for.
    void CloseGamepadView()
    {
        if (gamepad_view_ && pad_slot_ < kGamepadSlots)
        {
            const auto& name = ProfileAt(edit_profile_).name;
            if (draft_.pad_profile_names[pad_slot_] != name)
            {
                draft_.pad_profile_names[pad_slot_] = name;
                dirty_ = true;
            }
        }
        gamepad_view_ = false;
        renaming_ = confirm_delete_ = false;
        pad_capture_action_ = pad_capture_slot_ = -1;
        if (dirty_ && config_ok_) Save();
    }

    // Profile `index` (-1: the built-in Default).
    const Settings::PadProfile& ProfileAt(int index) const
    {
        return index < 0 ? Settings::DefaultPadProfile() : draft_.pad_profiles[std::size_t(index)];
    }

    // The profile kind of gamepad `slot`'s connected controller, or "" when it
    // is not connected.
    std::string SlotKind(std::size_t slot) const
    {
        return slot < pads_.size() ? platform::GamepadProfileKind(platform::GamepadFamilyOf(PadType(int(slot))))
                                   : std::string();
    }

    // The page follows its gamepad's controller; without one connected it
    // follows the profile on screen.
    std::string PageKind() const
    {
        const auto kind = SlotKind(pad_slot_);
        return !kind.empty() ? kind : ProfileAt(edit_profile_).controller;
    }
    platform::GamepadFamily PageFamily() const
    {
        return pad_slot_ < pads_.size() ? platform::GamepadFamilyOf(PadType(int(pad_slot_)))
                                        : platform::GamepadFamilyOfKind(PageKind());
    }
    // Button names: exactly the connected controller's (Cross on a DualSense).
    SDL_GamepadType PageType() const
    {
        return pad_slot_ < pads_.size() ? PadType(int(pad_slot_)) : platform::GamepadFamilyType(PageFamily());
    }

    // Profile `index` as shown: the built-in Default in the page controller's layout.
    Settings::PadProfile ShownProfile(int index) const
    {
        auto profile = ProfileAt(index);
        if (index < 0) profile.inputs = platform::DefaultGamepadInputs(PageFamily());
        return profile;
    }

    std::string ProfileUsers(int index) const
    {
        std::string users;
        for (std::size_t slot = 0; slot < kGamepadSlots; ++slot)
            if (draft_.PadProfileIndex(slot) == index)
                users += (users.empty() ? "" : ", ") + std::string("Gamepad ") + std::to_string(slot + 1);
        return users;
    }

    // A free name like "Profile 2".
    std::string NewProfileName() const
    {
        for (int n = int(draft_.pad_profiles.size()) + 1;; ++n)
        {
            const auto name = "Profile " + std::to_string(n);
            if (std::none_of(draft_.pad_profiles.begin(), draft_.pad_profiles.end(),
                             [&](const auto& profile) { return SamePadProfileName(profile.name, name); }))
                return name;
        }
    }

    // Selectables for Default and the profiles of `kind` (every profile, with
    // its kind, when `kind` is empty), plus `current`; returns the chosen
    // index (-1 Default) or -2 when nothing was chosen.
    int ProfileOptions(int current, const std::string& kind)
    {
        int chosen = -2;
        for (int n = -1; n < int(draft_.pad_profiles.size()); ++n)
        {
            const auto& profile = ProfileAt(n);
            if (n >= 0 && n != current && !kind.empty() && profile.controller != kind) continue;
            ImGui::PushID(n);
            std::string label = n < 0 ? std::string(kDefaultGamepadProfile) + " (built-in)" : profile.name;
            if (n >= 0 && (kind.empty() || profile.controller != kind))
                label += std::string("  (") + platform::GamepadKindName(profile.controller) + ")";
            if (ImGui::Selectable(label.c_str(), n == current)) chosen = n;
            ImGui::PopID();
        }
        return chosen;
    }

    // Gamepad presses while an input waits for one: a button, a trigger
    // pressed past half its travel or a stick pushed one way past half its
    // travel. Escape stops waiting.
    void GamepadCaptureEvent(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.axis <= SDL_GAMEPAD_AXIS_RIGHTY)
            if (SDL_Gamepad* pad = SDL_GetGamepadFromID(event.gaxis.which))
            {
                const int first = event.gaxis.axis >= SDL_GAMEPAD_AXIS_RIGHTX ? platform::RightStickUp
                                                                              : platform::LeftStickUp;
                auto& held = stick_held_[event.gaxis.which];
                for (int direction = first; direction < first + 4; ++direction)
                {
                    const auto input = platform::kGamepadStickBase + direction;
                    const bool pressed = platform::GamepadStickDirectionHeld(pad, input, held[direction]);
                    if (pressed && !held[direction] && pad_capture_action_ >= 0) CaptureGamepadInput(input);
                    held[direction] = pressed;
                    TrackPadInput(event.gaxis.which, input, pressed);
                }
            }
        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                            || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        {
            auto& held = trigger_held_[event.gaxis.which][event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
            const bool pressed = event.gaxis.value > platform::kGamepadTriggerPressed;
            if (pressed && !held && pad_capture_action_ >= 0)
                CaptureGamepadInput(platform::kGamepadTriggerBase + event.gaxis.axis);
            held = pressed;
            TrackPadInput(event.gaxis.which, platform::kGamepadTriggerBase + event.gaxis.axis, pressed);
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
            TrackPadInput(event.gbutton.which, event.gbutton.button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
            for (auto it = pad_inputs_held_.begin(); it != pad_inputs_held_.end();)
                it = it->first == event.gdevice.which ? pad_inputs_held_.erase(it) : std::next(it);
        if (pad_capture_action_ < 0) return;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) CaptureGamepadInput(event.gbutton.button);
        else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
            pad_capture_action_ = pad_capture_slot_ = -1;
    }

    // Gamepad buttons and triggers held down, to keep a captured press away
    // from the launcher's navigation until it is released.
    void TrackPadInput(SDL_JoystickID pad, platform::GamepadInput input, bool down)
    {
        if (down) pad_inputs_held_.insert({pad, input});
        else pad_inputs_held_.erase({pad, input});
    }

    // A press while an input waits: it takes that slot, moving it from another
    // action of the profile unless it is that action's only input.
    void CaptureGamepadInput(platform::GamepadInput input)
    {
        const int action = pad_capture_action_, slot = pad_capture_slot_;
        pad_capture_action_ = pad_capture_slot_ = -1;
        pad_capture_release_ = true;
        if (action < 0 || edit_profile_ < 0 || platform::GamepadInputName(input).empty()) return;
        auto& profile = draft_.pad_profiles[std::size_t(edit_profile_)];
        const std::string name = PadInputLabel(input, PageType());
        auto bindings = platform::ParseGamepadBindings(profile.inputs);
        auto& mine = bindings[std::size_t(action)];
        if (mine[0] == input || mine[1] == input) return;
        int moved = -1;
        for (std::size_t other = 0; other < GamepadActionCount; ++other)
        {
            if (int(other) == action) continue;
            auto& binding = bindings[other];
            for (int k = 0; k < 2; ++k)
            {
                if (binding[k] != input) continue;
                binding = {binding[1 - k], platform::kGamepadNone};
                moved = int(other);
            }
        }
        mine[slot] = input;
        if (mine[0] == platform::kGamepadNone) mine = {mine[1], platform::kGamepadNone};
        profile.inputs[std::size_t(action)] = platform::FormatGamepadBinding(mine);
        if (moved >= 0) profile.inputs[std::size_t(moved)] = platform::FormatGamepadBinding(bindings[moved]);
        dirty_ = true;
        const bool emptied = moved >= 0 && bindings[std::size_t(moved)][0] == platform::kGamepadNone;
        Notice(name + " now presses " + kGamepadActions[action].label
                   + (moved >= 0 ? std::string(" (moved from ") + kGamepadActions[moved].label
                                       + (emptied ? ", which now has no button)" : ")")
                                 : std::string())
                   + ".",
               emptied ? NoticeKind::Warning : NoticeKind::Info);
    }

    // A combo choosing the profile of gamepad `slot`: Default and the
    // profiles for its connected controller (all without one).
    void ProfileCombo(const char* id, std::size_t slot, float width)
    {
        ImGui::SetNextItemWidth(width);
        const int current = draft_.PadProfileIndex(slot);
        const auto kind = SlotKind(slot);
        const auto& profile = ProfileAt(current);
        const bool mismatch = current >= 0 && !kind.empty() && profile.controller != kind;
        if (ImGui::BeginCombo(id, profile.name.c_str()))
        {
            const int chosen = ProfileOptions(current, kind);
            if (chosen > -2)
            {
                draft_.pad_profile_names[slot] = ProfileAt(chosen).name;
                dirty_ = true;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen(id))
            ImGui::SetTooltip(mismatch ? "%s is made for %s controllers; this gamepad is another kind."
                                       : "Button profile of this gamepad%s",
                              mismatch ? profile.name.c_str() : "",
                              mismatch ? platform::GamepadKindName(profile.controller) : "");
    }

    void EditProfile(int index)
    {
        edit_profile_ = index;
        renaming_ = confirm_delete_ = false;
        pad_capture_action_ = pad_capture_slot_ = -1;
    }

    // A copy of the profile on screen, which the page then edits; false when
    // all profile slots are taken.
    bool CopyShownProfile()
    {
        auto& profiles = draft_.pad_profiles;
        if (profiles.size() >= kMaxGamepadProfiles) return false;
        auto copy = ShownProfile(edit_profile_);
        copy.name = NewProfileName();
        copy.controller = PageKind();
        profiles.push_back(copy);
        EditProfile(int(profiles.size()) - 1);
        dirty_ = true;
        return true;
    }

    // The built-in Default never changes: a change to it goes into a copy,
    // which the page's gamepad plays with from then on. Null when all
    // profile slots are taken.
    Settings::PadProfile* OwnCopyForChange()
    {
        if (!CopyShownProfile())
        {
            Notice("Default cannot change and all 16 profiles are in use. Delete one to make your own.",
                   NoticeKind::Warning);
            return nullptr;
        }
        auto& copy = draft_.pad_profiles.back();
        std::string user = "It is not assigned to a gamepad yet.";
        if (pad_slot_ < kGamepadSlots)
        {
            draft_.pad_profile_names[pad_slot_] = copy.name;
            user = "Gamepad " + std::to_string(pad_slot_ + 1) + " plays with it.";
        }
        Notice("Default cannot change, so your changes go into " + copy.name + ", a copy. " + user,
               NoticeKind::Info);
        return &copy;
    }

    void ProfileCard()
    {
        auto& profiles = draft_.pad_profiles;
        const bool builtin = edit_profile_ < 0;
        const std::string assigns = "Gamepad " + std::to_string(pad_slot_ + 1)
            + " plays with the profile shown here when you go back. Gamepads can share a profile; keep one per player.";
        BeginCard("##profile", "Profile", assigns.c_str(), Icon::Gamepad);
        if (BeginRows("##profile_rows"))
        {
            Row("Edit profile", nullptr);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (renaming_)
            {
                if (rename_focus_) { ImGui::SetKeyboardFocusHere(); rename_focus_ = false; }
                const bool done = ImGui::InputText("##rename", &rename_buffer_, ImGuiInputTextFlags_EnterReturnsTrue);
                if (done || ImGui::IsItemDeactivated())
                {
                    renaming_ = false;
                    auto& profile = profiles[std::size_t(edit_profile_)];
                    const auto name = rename_buffer_;
                    const bool taken = std::any_of(profiles.begin(), profiles.end(), [&](const auto& other) {
                        return &other != &profile && SamePadProfileName(other.name, name);
                    });
                    if (!ValidPadProfileName(name))
                        Notice("A profile name has 1 to 32 characters, without quotes, brackets, = or |, "
                               "and is not Default.", NoticeKind::Warning);
                    else if (taken) Notice("Another profile is already named " + name + ".", NoticeKind::Warning);
                    else if (name != profile.name)
                    {
                        for (auto& assigned : draft_.pad_profile_names)
                            if (SamePadProfileName(assigned, profile.name)) assigned = name;
                        profile.name = name;
                        dirty_ = true;
                    }
                }
            }
            else if (ImGui::BeginCombo("##edit_profile", ProfileAt(edit_profile_).name.c_str()))
            {
                const int chosen = ProfileOptions(edit_profile_, SlotKind(pad_slot_));
                if (chosen > -2) EditProfile(chosen);
                ImGui::EndCombo();
            }
            EndRows();
        }
        // Which controller the page follows.
        const std::string kind_name = platform::GamepadKindName(PageKind());
        const std::string controller = pad_slot_ < gamepad_names_.size()
            ? gamepad_names_[pad_slot_] + ": profiles for " + kind_name + " controllers."
            : "Gamepad " + std::to_string(pad_slot_ + 1) + " is not connected: all profiles are listed, buttons named for "
                + kind_name + " controllers.";
        TextColored(CurrentFonts().caption, color::muted, controller.c_str());
        const auto users = ProfileUsers(edit_profile_);
        TextColored(CurrentFonts().caption, color::muted,
                    ((builtin ? std::string("Built-in: changing a button below makes your own copy. ") : std::string())
                     + (users.empty() ? "No gamepad plays with this profile." : "Used by " + users + "."))
                        .c_str());
        ImGui::Dummy(Dp(0, 4));
        if (SecondaryButton("##new_profile", "New", Dp(110, 36), profiles.size() < kMaxGamepadProfiles, Icon::Copy)
            && CopyShownProfile())
            Notice(profiles.back().name + " starts as a copy of the profile you were editing.", NoticeKind::Info);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(profiles.size() < kMaxGamepadProfiles ? "A new profile, copied from this one"
                                                                    : "Up to 16 profiles");
        ImGui::SameLine(0, Dp(8));
        if (SecondaryButton("##rename_profile", "Rename", Dp(120, 36), !builtin && !renaming_, Icon::None))
        {
            renaming_ = rename_focus_ = true;
            confirm_delete_ = false;
            rename_buffer_ = profiles[std::size_t(edit_profile_)].name;
        }
        ImGui::SameLine(0, Dp(8));
        // Deleting asks once more; gamepads using the profile play with Default.
        if (SecondaryButton("##delete_profile", confirm_delete_ ? "Really delete?" : "Delete",
                            Dp(confirm_delete_ ? 170 : 120, 36), !builtin, Icon::Error))
        {
            if (!confirm_delete_) confirm_delete_ = true;
            else
            {
                const auto removed = profiles[std::size_t(edit_profile_)].name;
                profiles.erase(profiles.begin() + edit_profile_);
                for (auto& assigned : draft_.pad_profile_names)
                    if (SamePadProfileName(assigned, removed)) assigned = kDefaultGamepadProfile;
                EditProfile(-1);
                dirty_ = true;
                Notice("Deleted " + removed + ". Its gamepads now play with Default.", NoticeKind::Info);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(builtin ? "The built-in profile cannot be deleted"
                                      : confirm_delete_ ? "Click again to delete; gamepads using it switch to Default"
                                                        : "Delete this profile");
        else if (confirm_delete_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            confirm_delete_ = false;
        EndCard();
    }

    void GamepadView()
    {
        if (SecondaryButton("##back", "Players", Dp(124, 36), true, Icon::Back)) { CloseGamepadView(); return; }
        ImGui::Dummy(Dp(0, 6));
        PageHeader("Gamepad profiles", "Mario Strikers Charged is played with a Wii Remote and Nunchuk. Your "
                   "controller stands in for them: a profile says which of its buttons presses each Wii button. "
                   "Pick a profile per gamepad, here or on the Players list. To change a button, select it below "
                   "and press the new one on the controller. Changes are saved when you go back.");
        if (edit_profile_ >= int(draft_.pad_profiles.size())) edit_profile_ = -1;
        ProfileCard();
        // The built-in Default never changes: its first change makes an own copy.
        const bool builtin = edit_profile_ < 0;
        Settings::PadProfile scratch = ShownProfile(edit_profile_);
        auto& profile = builtin ? scratch : draft_.pad_profiles[std::size_t(edit_profile_)];
        const auto editable = [&]() -> Settings::PadProfile* { return builtin ? OwnCopyForChange() : &profile; };
        const auto type = PageType();
        const auto bindings = platform::ParseGamepadBindings(profile.inputs);
        bool clicked = false;
        const auto rows = [&](std::size_t first, std::size_t last) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
            if (ImGui::BeginTable("##pad_inputs", 2))
            {
                ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
                ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthStretch);
                for (std::size_t n = first; n < last; ++n)
                {
                    ImGui::PushID(int(n));
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, Dp(40));
                    ImGui::TableSetColumnIndex(0);
                    TextColored(CurrentFonts().label, color::text, kGamepadActions[n].label);
                    if (kGamepadActions[n].note)
                    {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
                        TextColored(CurrentFonts().caption, color::muted, kGamepadActions[n].note);
                    }
                    ImGui::TableSetColumnIndex(1);
                    for (int slot = 0; slot < 2; ++slot)
                    {
                        const auto input = bindings[n][slot];
                        if (slot == 1 && bindings[n][0] == platform::kGamepadNone) break;
                        const bool waiting = pad_capture_action_ == int(n) && pad_capture_slot_ == slot;
                        const std::string label = waiting ? "Press a button..."
                            : input != platform::kGamepadNone ? PadInputLabel(input, type)
                            : slot == 0 ? "Not set" : "+";
                        if (slot) ImGui::SameLine(0, Dp(6));
                        ImGui::PushID(slot);
                        if (KeyCapButton("##input", label.c_str(), waiting) && (waiting || editable()))
                        {
                            clicked = true;
                            pad_capture_action_ = waiting ? -1 : int(n);
                            pad_capture_slot_ = waiting ? -1 : slot;
                        }
                        if (ImGui::IsItemHovered() && !waiting)
                            ImGui::SetTooltip(input != platform::kGamepadNone
                                                  ? "Select, then press the new button or push a stick on the controller"
                                                  : slot == 0 ? "No button presses this. Select, then press one."
                                                              : "Add a second button");
                        ImGui::PopID();
                    }
                    if (bindings[n][0] != platform::kGamepadNone)
                    {
                        // Removes the second button, or the only one (no button then).
                        const bool two = bindings[n][1] != platform::kGamepadNone;
                        ImGui::SameLine(0, Dp(6));
                        if (SecondaryButton("##remove", "", Dp(30, 30), true, Icon::Error))
                            if (auto* target = editable())
                            {
                                target->inputs[n] = platform::FormatGamepadBinding(
                                    {two ? bindings[n][0] : platform::kGamepadNone, platform::kGamepadNone});
                                dirty_ = true;
                            }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(two ? "Remove the second button" : "Remove the button (none presses this)");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        };
        const std::string remote_title = "Wii Remote buttons: " + profile.name;
        BeginCard("##pad_remote", remote_title.c_str(),
                  "Each Wii Remote button, and the controller button that presses it.", Icon::Gamepad);
        rows(GamepadActionA, GamepadActionC);
        EndCard();
        BeginCard("##pad_nunchuk", "Nunchuk & motion",
                  "The Nunchuk stick and pointer on the controller's sticks or gyro, the Nunchuk buttons and "
                  "the shakes.",
                  Icon::Gamepad);
        const bool swap = profile.swap_sticks;
        const bool gyro = profile.gyro_pointer;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
        if (ImGui::BeginTable("##pad_sticks", 2))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
            ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthStretch);
            KeyRow("Nunchuk stick", {swap ? "Right stick" : "Left stick"}, nullptr, "Move, aim");
            KeyRow("Pointer", {gyro ? "Gyro" : swap ? "Left stick" : "Right stick"}, nullptr,
                   "Point at menu items and Mega Strike shots");
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        if (BeginRows("##pad_swap_rows"))
        {
            Row("Swap sticks", "Nunchuk stick on the right stick, pointer on the left.");
            bool swapped = swap;
            if (Toggle("##swap_sticks", &swapped))
                if (auto* target = editable())
                {
                    target->swap_sticks = swapped;
                    dirty_ = true;
                }
            // Gyro pointer (issue #14).
            Row("Gyro pointer", "Point by turning the controller, like a Wii Remote, instead of with the stick. "
                "For controllers with a gyro, such as PlayStation and Switch Pro controllers.");
            bool gyro_on = gyro;
            if (Toggle("##gyro_pointer", &gyro_on))
                if (auto* target = editable())
                {
                    target->gyro_pointer = gyro_on;
                    dirty_ = true;
                }
            if (gyro)
            {
                BeginSubRows();
                Row("Gyro speed", "At 100%, turning the controller about 25 degrees moves the pointer across "
                    "the picture.");
                int speed = profile.gyro_speed;
                if (PercentSlider("##gyro_speed", &speed, ImGui::GetContentRegionAvail().x, nullptr, kMinGyroSpeed,
                                  kMaxGyroSpeed)
                    && speed != profile.gyro_speed)
                    if (auto* target = editable())
                    {
                        target->gyro_speed = speed;
                        dirty_ = true;
                    }
                // The stick the gyro frees can press the D-pad.
                const int free_stick = swap ? platform::LeftStickUp : platform::RightStickUp;
                bool stick_dpad = true;
                for (int n = 0; n < 4; ++n)
                {
                    const auto& binding = bindings[GamepadActionUp + std::size_t(n)];
                    const auto input = platform::kGamepadStickBase + free_stick + n;
                    stick_dpad = stick_dpad && (binding[0] == input || binding[1] == input);
                }
                const std::string stick_dpad_label = std::string(swap ? "Left" : "Right") + " stick as D-pad";
                Row(stick_dpad_label.c_str(), "Push it to press the D-pad: in a match, a deke with the ball and a "
                    "slide tackle without it.");
                if (Toggle("##stick_dpad", &stick_dpad))
                    if (auto* target = editable())
                    {
                        SetStickDpad(*target, free_stick, stick_dpad);
                        dirty_ = true;
                    }
                EndSubRows();
            }
            EndRows();
        }
        if (gyro)
        {
            rows(GamepadActionRecenterPointer, GamepadActionCount);
            GyroPreview(profile, bindings);
        }
        rows(GamepadActionC, GamepadActionRecenterPointer);
        // A stick's directions bound to buttons while it also moves the
        // Nunchuk stick or the pointer do both at once.
        const auto stick_bound = [&](int first) {
            for (const auto& binding : bindings)
                for (const auto input : binding)
                    if (input >= platform::kGamepadStickBase + first && input < platform::kGamepadStickBase + first + 4)
                        return true;
            return false;
        };
        const int nunchuk_stick = swap ? platform::RightStickUp : platform::LeftStickUp;
        const int pointer_stick = swap ? platform::LeftStickUp : platform::RightStickUp;
        std::string stick_note;
        if (stick_bound(nunchuk_stick))
            stick_note = std::string(swap ? "The right" : "The left")
                + " stick moves the Nunchuk stick and presses buttons too: pushing it does both.";
        else if (!gyro && stick_bound(pointer_stick))
            stick_note = std::string(swap ? "The left" : "The right")
                + " stick moves the pointer and presses buttons too: pushing it does both. "
                  "With the gyro pointer on, it only presses the buttons.";
        if (!stick_note.empty()) TextWrappedColored(CurrentFonts().caption, color::warning, stick_note.c_str());
        EndCard();

        // Dead zones and trigger press point (issue #62), tried live on the
        // page's gamepad.
        BeginCard("##pad_deadzones", "Dead zones & triggers",
                  "How far a stick or trigger must move before it counts. Try it on the controller below.",
                  Icon::Sliders);
        if (BeginRows("##pad_deadzone_rows"))
        {
            const auto slider = [&](const char* label, const char* description, const char* id, int value,
                                    int minimum, int maximum, int Settings::PadProfile::*field) {
                Row(label, description);
                if (PercentSlider(id, &value, ImGui::GetContentRegionAvail().x, nullptr, minimum, maximum)
                    && value != profile.*field)
                    if (auto* target = editable())
                    {
                        target->*field = value;
                        dirty_ = true;
                    }
            };
            slider("Inner dead zone", "The stick rests until it is tilted this far. Raise it if a worn stick drifts.",
                   "##inner_deadzone", profile.inner_deadzone, kMinInnerDeadzone, kMaxInnerDeadzone,
                   &Settings::PadProfile::inner_deadzone);
            slider("Outer dead zone", "From this tilt on the stick is pushed all the way. 71% plays like a Nunchuk "
                   "on the Wii; higher leaves more room for fine moves and slow pointing.",
                   "##outer_deadzone", profile.outer_deadzone, kMinOuterDeadzone, kMaxOuterDeadzone,
                   &Settings::PadProfile::outer_deadzone);
            slider("Trigger press point", "A trigger presses its button past this much of its travel.",
                   "##trigger_press", profile.trigger_press, kMinTriggerPress, kMaxTriggerPress,
                   &Settings::PadProfile::trigger_press);
            EndRows();
        }
        DeadzonePreview(profile);
        // Defaults of the controller shown above.
        const auto defaults = platform::DefaultGamepadInputs(PageFamily());
        const Settings::PadProfile default_profile;
        const bool changed = platform::ParseGamepadBindings(profile.inputs) != platform::ParseGamepadBindings(defaults)
            || swap || profile.inner_deadzone != default_profile.inner_deadzone
            || profile.outer_deadzone != default_profile.outer_deadzone
            || profile.trigger_press != default_profile.trigger_press
            || profile.gyro_pointer != default_profile.gyro_pointer || profile.gyro_speed != default_profile.gyro_speed;
        ImGui::Dummy(Dp(0, 4));
        if (SecondaryButton("##reset_pad", "Reset to defaults", Dp(180, 36), changed, Icon::Refresh))
        {
            profile.inputs = defaults;
            profile.swap_sticks = false;
            profile.inner_deadzone = default_profile.inner_deadzone;
            profile.outer_deadzone = default_profile.outer_deadzone;
            profile.trigger_press = default_profile.trigger_press;
            profile.gyro_pointer = default_profile.gyro_pointer;
            profile.gyro_speed = default_profile.gyro_speed;
            pad_capture_action_ = pad_capture_slot_ = -1;
            dirty_ = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Buttons, sticks, gyro, dead zones and triggers as in Default");
        EndCard();
        // A click anywhere else stops waiting for a press.
        if (pad_capture_action_ >= 0 && !clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            pad_capture_action_ = pad_capture_slot_ = -1;
    }

    // Adds a stick's four directions to the D-pad's inputs, after the D-pad's
    // own (a direction bound elsewhere moves here), or takes them away again;
    // a D-pad action left without input gets its default back.
    void SetStickDpad(Settings::PadProfile& profile, int first_direction, bool on)
    {
        const auto before = platform::ParseGamepadBindings(profile.inputs);
        const auto defaults = platform::ParseGamepadBindings(platform::DefaultGamepadInputs(PageFamily()));
        auto bindings = before;
        for (int n = 0; n < 4; ++n)
        {
            const auto input = platform::kGamepadStickBase + first_direction + n;
            for (auto& binding : bindings)
            {
                if (binding[1] == input) binding[1] = platform::kGamepadNone;
                if (binding[0] == input) binding = {binding[1], platform::kGamepadNone};
            }
            const std::size_t action = GamepadActionUp + std::size_t(n);
            auto& dpad = bindings[action];
            if (on) dpad = dpad[0] != platform::kGamepadNone ? platform::GamepadBinding{dpad[0], input}
                                                             : platform::GamepadBinding{input, platform::kGamepadNone};
            else if (dpad[0] == platform::kGamepadNone) dpad = defaults[action];
        }
        for (std::size_t action = 0; action < GamepadActionCount; ++action)
            if (bindings[action] != before[action]) profile.inputs[action] = platform::FormatGamepadBinding(bindings[action]);
    }

    // The page's gamepad turning a pointer over a picture-shaped box at the
    // profile's gyro speed, as in the game; its recentre button or a click
    // centres it. Without a gyro the stick points, as in the game.
    void GyroPreview(const Settings::PadProfile& profile, const platform::GamepadBindings& bindings)
    {
        gyro_preview_drawn_ = true;
        const auto& fonts = CurrentFonts();
        SDL_Gamepad* pad = pad_slot_ < pads_.size() ? pads_[pad_slot_] : nullptr;
        if (!pad)
        {
            TextWrappedColored(fonts.caption, color::muted,
                               ("Connect gamepad " + std::to_string(pad_slot_ + 1) + " to try the gyro here.").c_str());
            return;
        }
        const std::string name = pad_slot_ < gamepad_names_.size() ? gamepad_names_[pad_slot_] : "This controller";
        if (!SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO))
        {
            TextWrappedColored(fonts.caption, color::warning,
                               (name + " has no gyro: its " + (profile.swap_sticks ? "left" : "right")
                                + " stick moves the pointer instead.")
                                   .c_str());
            return;
        }
        if (gyro_preview_pad_ != pad)
        {
            StopGyroPreview();
            SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            gyro_preview_pad_ = pad;
        }
        const auto& recenter_binding = bindings[GamepadActionRecenterPointer];
        const bool recenter = platform::GamepadBindingHeld(pad, recenter_binding, profile.trigger_press);
        const Uint64 now = SDL_GetTicksNS();
        const float seconds = gyro_preview_ns_ ? float(double(now - gyro_preview_ns_) * 1e-9) : 0.0f;
        gyro_preview_ns_ = now;
        const auto turn = platform::TakeGyroTurn(gyro_preview_motion_, seconds);
        if (recenter && !gyro_preview_recenter_held_) CentreGyroPreview();
        else platform::MoveGyroPointer(gyro_preview_x_, gyro_preview_y_, turn, profile.gyro_speed, 9.0f / 16.0f);
        gyro_preview_recenter_held_ = recenter;
        ImGui::Dummy(Dp(0, 8));
        const float width = std::min(ImGui::GetContentRegionAvail().x, Dp(320)), height = width * 9.0f / 16.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos(), end{origin.x + width, origin.y + height};
        if (ImGui::InvisibleButton("##gyro_preview", {width, height})) CentreGyroPreview();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to centre the pointer");
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, end, Col(color::field), Dp(8));
        // Hairlines as thin rectangles: SDL's software renderer drops parts of long lines.
        const float hair = std::max(1.0f, Dp(1)), cx = origin.x + width * 0.5f, cy = origin.y + height * 0.5f;
        draw->AddRectFilled({cx - hair * 0.5f, origin.y}, {cx + hair * 0.5f, end.y}, Col(color::border));
        draw->AddRectFilled({origin.x, cy - hair * 0.5f}, {end.x, cy + hair * 0.5f}, Col(color::border));
        draw->AddRect(origin, end, Col(color::border), Dp(8), 0, Dp(1.5f));
        const ImVec2 dot{origin.x + gyro_preview_x_ * width, origin.y + gyro_preview_y_ * height};
        draw->AddCircle(dot, Dp(11), Col(color::accent, 0.45f), 24, Dp(1.5f));
        draw->AddCircleFilled(dot, Dp(6), Col(color::accent_hi));
        const std::string centre = recenter_binding[0] != platform::kGamepadNone
            ? PadInputLabel(recenter_binding[0], SDL_GetGamepadType(pad)) + " or a click centres it."
            : "A click centres it.";
        TextWrappedColored(fonts.caption, color::muted, ("Turn " + name + " to move the pointer. " + centre).c_str());
    }

    // Readings of the gamepad whose gyro the preview shows.
    void GyroPreviewEvent(const SDL_Event& event)
    {
        if (event.type != SDL_EVENT_GAMEPAD_SENSOR_UPDATE || !gyro_preview_pad_
            || event.gsensor.which != SDL_GetGamepadID(gyro_preview_pad_))
            return;
        if (event.gsensor.sensor == SDL_SENSOR_GYRO)
            platform::AddGyroReading(gyro_preview_motion_, event.gsensor.data, event.gsensor.sensor_timestamp);
        else if (event.gsensor.sensor == SDL_SENSOR_ACCEL)
            platform::AddAccelReading(gyro_preview_motion_, event.gsensor.data, event.gsensor.sensor_timestamp);
    }

    // Turns the preview's sensors off; it starts again centred.
    void StopGyroPreview()
    {
        if (gyro_preview_pad_)
        {
            SDL_SetGamepadSensorEnabled(gyro_preview_pad_, SDL_SENSOR_GYRO, false);
            SDL_SetGamepadSensorEnabled(gyro_preview_pad_, SDL_SENSOR_ACCEL, false);
        }
        gyro_preview_pad_ = nullptr;
        gyro_preview_motion_ = {};
        gyro_preview_ns_ = 0;
        gyro_preview_x_ = gyro_preview_y_ = 0.5f;
        gyro_preview_recenter_held_ = false;
    }

    // Puts the preview's pointer back in the middle.
    void CentreGyroPreview()
    {
        gyro_preview_x_ = gyro_preview_y_ = 0.5f;
        platform::ClearGyroTurn(gyro_preview_motion_);
    }

    // Both sticks and triggers of the page's gamepad, live, with `profile`'s
    // dead zones and press point: the shaded centre is the inner dead zone,
    // the ring the outer one; the figure is how far the game sees the stick
    // pushed.
    void DeadzonePreview(const Settings::PadProfile& profile)
    {
        const auto bindings = platform::ParseGamepadBindings(profile.inputs);
        SDL_Gamepad* pad = pad_slot_ < pads_.size() ? pads_[pad_slot_] : nullptr;
        if (!pad)
        {
            TextWrappedColored(CurrentFonts().caption, color::muted,
                               ("Connect gamepad " + std::to_string(pad_slot_ + 1) + " to try these here.").c_str());
            return;
        }
        ImGui::Dummy(Dp(0, 8));
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const float radius = Dp(52), bar_w = Dp(26), caption = Dp(40), gap = Dp(28);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const auto axis = [&](SDL_GamepadAxis which) {
            return std::clamp(float(SDL_GetGamepadAxis(pad, which)) / 32767.0f, -1.0f, 1.0f);
        };
        const auto centred = [&](float centre_x, float y, ImFont* font, const ImVec4& tint, const std::string& text) {
            const ImVec2 size = TextSize(font, text.c_str());
            DrawLabel(draw, font, {centre_x - size.x * 0.5f, y}, Col(tint), text.c_str());
        };
        const auto stick = [&](float left, bool left_stick, bool nunchuk) {
            const ImVec2 c{left + radius, origin.y + radius};
            const auto x_axis = left_stick ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
            const auto y_axis = left_stick ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
            const auto tilt = platform::GamepadStickTilt(pad, x_axis, y_axis, profile.inner_deadzone,
                                                         profile.outer_deadzone);
            const float pushed = std::min(1.0f, std::hypot(tilt.x, tilt.y));
            draw->AddCircleFilled(c, radius, Col(color::field), 48);
            draw->AddCircleFilled(c, radius, Col(color::accent, 0.10f), 48);
            draw->AddCircleFilled(c, radius * profile.outer_deadzone / 100.0f, Col(color::field), 48);
            draw->AddCircle(c, radius * profile.outer_deadzone / 100.0f, Col(color::accent, 0.55f), 48, Dp(1.5f));
            // Hairlines as thin rectangles: SDL's software renderer drops parts of long lines.
            const float hair = std::max(1.0f, Dp(1));
            draw->AddRectFilled({c.x - radius, c.y - hair * 0.5f}, {c.x + radius, c.y + hair * 0.5f}, Col(color::border));
            draw->AddRectFilled({c.x - hair * 0.5f, c.y - radius}, {c.x + hair * 0.5f, c.y + radius}, Col(color::border));
            if (profile.inner_deadzone > 0)
                draw->AddCircleFilled(c, radius * profile.inner_deadzone / 100.0f, Col(color::dim, 0.45f), 32);
            draw->AddCircle(c, radius, Col(color::border), 48, Dp(1.5f));
            float x = axis(x_axis), y = axis(y_axis);
            if (const float length = std::hypot(x, y); length > 1.0f) { x /= length; y /= length; }
            const ImVec4 dot = pushed <= 0.0f ? color::muted : pushed >= 1.0f ? color::accent_hi : color::accent;
            draw->AddCircleFilled({c.x + x * radius, c.y + y * radius}, Dp(6), Col(dot));
            bool buttons = false;
            for (const auto& binding : bindings)
                for (const auto input : binding)
                    buttons = buttons
                        || (platform::IsGamepadStickDirection(input)
                            && (input - platform::kGamepadStickBase >= platform::RightStickUp) != left_stick);
            const std::string name = std::string(left_stick ? "Left stick" : "Right stick") + ": "
                + (nunchuk ? "Nunchuk" : !profile.gyro_pointer ? "pointer" : buttons ? "buttons" : "not used");
            centred(c.x, origin.y + radius * 2 + Dp(6), fonts.caption, color::muted, name);
            centred(c.x, origin.y + radius * 2 + Dp(22), fonts.caption, pushed > 0 ? color::text : color::dim,
                    std::to_string(int(std::lround(pushed * 100))) + "% pushed");
        };
        const auto trigger = [&](float left, SDL_GamepadAxis which) {
            const float top = origin.y, bottom = origin.y + radius * 2;
            const float value = std::clamp(float(SDL_GetGamepadAxis(pad, which)) / 32767.0f, 0.0f, 1.0f);
            const bool pressed = SDL_GetGamepadAxis(pad, which) > platform::GamepadTriggerThreshold(profile.trigger_press);
            draw->AddRectFilled({left, top}, {left + bar_w, bottom}, Col(color::field), Dp(6));
            if (value > 0)
                draw->AddRectFilled({left, bottom - (bottom - top) * value}, {left + bar_w, bottom},
                                    Col(pressed ? color::accent : color::muted, pressed ? 0.85f : 0.45f), Dp(6));
            const float mark = bottom - (bottom - top) * profile.trigger_press / 100.0f;
            draw->AddRectFilled({left - Dp(4), mark - Dp(1)}, {left + bar_w + Dp(4), mark + Dp(1)}, Col(color::accent_hi));
            draw->AddRect({left, top}, {left + bar_w, bottom}, Col(color::border), Dp(6), 0, Dp(1.5f));
            const auto name = PadInputLabel(platform::kGamepadTriggerBase + which, SDL_GetGamepadType(pad));
            centred(left + bar_w * 0.5f, origin.y + radius * 2 + Dp(6), fonts.caption, color::muted, name);
            centred(left + bar_w * 0.5f, origin.y + radius * 2 + Dp(22), fonts.caption,
                    pressed ? color::accent : color::dim, pressed ? "on" : "off");
        };
        float left = origin.x + Dp(4);
        stick(left, true, !profile.swap_sticks);
        left += radius * 2 + gap;
        stick(left, false, profile.swap_sticks);
        left += radius * 2 + gap;
        trigger(left, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        left += bar_w + gap;
        trigger(left, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        ImGui::Dummy({left + bar_w - origin.x, radius * 2 + caption});
    }

    void ControlsMain()
    {
        PageHeader("Controls", "Play with keyboard and mouse, an Xbox, PlayStation or other gamepad, or a Wii Remote with Nunchuk. Keep the game window focused.");
        PlayersCard();
        WiiRemoteCard();
        RumbleCard();
        HotkeysCard();

        BeginCard("##controller", "Controller test", "Check a gamepad's buttons and sticks. Assign gamepads to players above.", Icon::Gamepad);
        if (!gamepad_)
            TextWrappedColored(CurrentFonts().body, color::muted, "No controller detected.");
        else
        {
            const char* name = SDL_GetGamepadName(gamepad_);
            Chip(name ? name : "Connected controller", color::accent, Icon::Gamepad);
            const float x = float(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX)) / 32768.0f;
            const float y = float(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY)) / 32768.0f;
            const std::string stick = "Left stick  X " + std::to_string(int(std::lround(x * 100))) + "%   Y "
                + std::to_string(int(std::lround(y * 100))) + "%";
            TextColored(CurrentFonts().body, color::text, stick.c_str());
            const SDL_GamepadButton buttons[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
            const char* labels[] = {"South", "East", "West", "North"};
            for (int i = 0; i < 4; ++i)
            {
                if (i) ImGui::SameLine(0, Dp(8));
                Chip(labels[i], SDL_GetGamepadButton(gamepad_, buttons[i]) ? color::accent : color::dim);
            }
        }
        EndCard();
    }

    // The Super Team needs a Super Mario Strikers (GameCube) image: one of the
    // game folder is taken when none is set; turning the tweak on without one
    // asks for it.
    void PrefillGameCubeDisc()
    {
        if (!draft_.gamecube_disc.empty() || library_strikers_.empty()) return;
        const bool clean = !dirty_;
        draft_.gamecube_disc = SettingsPath(library_strikers_.front());
        dirty_ = true;
        if (clean) Save();
        if (draft_.tweaks[GameTweakIndex(GameTweak::SuperTeam)])
            Notice("The Super Team uses " + PathUtf8(library_strikers_.front().filename()) + " from your game folder.",
                   NoticeKind::Success);
    }

    void BrowseGameCubeDisc()
    {
        static const SDL_DialogFileFilter filters[] = {
            {"GameCube disc images (ISO / RVZ / GCM)", "iso;rvz;gcm;ISO;RVZ;GCM"}, {"All files", "*"}};
        {
            std::lock_guard<std::mutex> lock(gamecube_dialog_->mutex);
            gamecube_dialog_->active = true;
        }
        const fs::path current = draft_.gamecube_disc.empty() ? fs::path()
                                                              : ResolveGameCubeDiscPath(draft_, file_.path);
        const fs::path start = !current.empty() ? current.parent_path() : ResolveGameFolder(draft_, file_.path);
        const std::string location = PathUtf8(start);
        SDL_ShowOpenFileDialog(DiscChosen, new std::shared_ptr<DialogResult>(gamecube_dialog_), window_, filters,
                               int(std::size(filters)), location.empty() ? nullptr : location.c_str(), false);
    }

    void GameCubeDiscChosen(const fs::path& path)
    {
        const DiscProbe probe = ProbeDisc(path);
        if (probe.kind != DiscProbe::Kind::OtherGame || !IsStrikersGameId(probe.info.game_id))
        {
            Notice(PathUtf8(path.filename()) + " is not a Super Mario Strikers (GameCube) image.", NoticeKind::Error);
            return;
        }
        draft_.gamecube_disc = SettingsPath(path);
        dirty_ = true;
        Notice("The Super Team uses " + PathUtf8(path.filename()) + ".", NoticeKind::Success);
    }

    void SuperTeamRows()
    {
        const auto index = GameTweakIndex(GameTweak::SuperTeam);
        const auto& info = kGameTweaks[index];
        Row(info.name, info.description);
        if (Toggle("##tweak_super_team", &draft_.tweaks[index]))
        {
            dirty_ = true;
            if (draft_.tweaks[index] && draft_.gamecube_disc.empty()) BrowseGameCubeDisc();
        }
        if (!draft_.tweaks[index]) return;
        BeginSubRows();
        Row("Side", "The team whose captain and sidekicks are robots.");
        const char* sides[] = {"away", "home", "both"};
        int side = 0;
        for (int i = 0; i < 3; ++i)
            if (draft_.super_team_side == sides[i]) side = i;
        if (Segmented("##super_team_side", &side, {"Away team", "Home team", "Both teams"},
                      ImGui::GetContentRegionAvail().x))
        {
            draft_.super_team_side = sides[side];
            dirty_ = true;
        }
        bool browsing;
        {
            std::lock_guard<std::mutex> lock(gamecube_dialog_->mutex);
            browsing = gamecube_dialog_->active;
        }
        const std::string path = draft_.gamecube_disc.empty()
            ? std::string("None found in your game folder. Choose your Super Mario Strikers image.")
            : PathUtf8(ResolveGameCubeDiscPath(draft_, file_.path));
        Row("GameCube disc", path.c_str());
        const std::string label = draft_.gamecube_disc.empty()
            ? std::string("Choose image...")
            : PathUtf8(PathFromUtf8(draft_.gamecube_disc).filename());
        if (SecondaryButton("gamecube_disc", label.c_str(), {ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()},
                            !browsing && config_ok_, Icon::Disc))
            BrowseGameCubeDisc();
        EndSubRows();
    }

    void TweakRow(GameTweak tweak)
    {
        const auto index = GameTweakIndex(tweak);
        const auto& info = kGameTweaks[index];
        Row(info.name, info.description);
        const std::string id = std::string("##tweak_") + info.key;
        if (Toggle(id.c_str(), &draft_.tweaks[index])) dirty_ = true;
    }

    void GameplayPage()
    {
        const ImVec2 header = ImGui::GetCursorScreenPos();
        const float right = header.x + ImGui::GetContentRegionAvail().x;
        PageHeader("Gameplay", "Optional tweaks that change the original game. With all of them off, the game plays "
                               "as on the Wii.");
        const ImVec2 content = ImGui::GetCursorScreenPos();
        const auto& fonts = CurrentFonts();
        const ImVec2 label = TextSize(fonts.body, "All tweaks");
        DrawLabel(ImGui::GetWindowDrawList(), fonts.body,
                  {right - Dp(60) - label.x, header.y + (FontSize(fonts.title) - label.y) * 0.5f},
                  Col(color::muted), "All tweaks");
        ImGui::SetCursorScreenPos({right - Dp(48), header.y + (FontSize(fonts.title) - Dp(28)) * 0.5f});
        bool all_enabled = std::all_of(draft_.tweaks.begin(), draft_.tweaks.end(), [](bool enabled) { return enabled; });
        if (Toggle("##all_tweaks", &all_enabled))
        {
            draft_.tweaks.fill(all_enabled);
            dirty_ = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn all gameplay tweaks on or off.");
        ImGui::SetCursorScreenPos(content);

        BeginCard("##menus", "Menus & startup", nullptr, Icon::Play);
        if (BeginRows("##menus_rows"))
        {
            TweakRow(GameTweak::SkipIntro);
            TweakRow(GameTweak::FastMenus);
            EndRows();
        }
        EndCard();

        BeginCard("##matches", "Matches & unlocks", nullptr, Icon::Target);
        if (BeginRows("##matches_rows"))
        {
            TweakRow(GameTweak::CpuVsCpu);
            TweakRow(GameTweak::MirrorMatch);
            TweakRow(GameTweak::BluePeach);
            TweakRow(GameTweak::RobotGoalies);
            SuperTeamRows();
            TweakRow(GameTweak::UnlockAll);
            EndRows();
        }
        EndCard();

        BeginCard("##competitive", "Competitive gameplay", nullptr, Icon::Trophy);
        if (BeginRows("##competitive_rows"))
        {
            TweakRow(GameTweak::FastTerrain);
            TweakRow(GameTweak::ThunderIslandWalls);
            TweakRow(GameTweak::FairBallContests);
            TweakRow(GameTweak::NoMegaStrikes);
            EndRows();
        }
        EndCard();

        ModsCard();
    }

    // ------------------------------------------------------- texture packs
    // The packs in the texture packs folder, read in the background when the
    // Gameplay page opens, the folder changes, a pack is added or Refresh is chosen.
    void PollTexturePacks()
    {
        if (texture_scanning_ && texture_scan_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            texture_packs_ = texture_scan_.get();
            texture_scanning_ = false;
        }
        if (pack_copy_.valid() && pack_copy_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            const std::string error = pack_copy_.get();
            if (error.empty()) Notice(pack_copy_name_ + " was added to your texture packs.", NoticeKind::Success);
            else Notice(error, NoticeKind::Error);
            pack_copy_name_.clear();
            texture_rescan_ = true;
        }
        const bool opened = !texture_page_open_;
        texture_page_open_ = true;
        const auto folder = TextureFolder();
        if (opened || PathUtf8(folder) != texture_scanned_folder_) texture_rescan_ = true;
        if (texture_rescan_ && !texture_scanning_)
        {
            texture_rescan_ = false;
            texture_scanned_folder_ = PathUtf8(folder);
            texture_scanning_ = true;
            texture_scan_ = std::async(std::launch::async, [folder] { return platform::FindTexturePacks(folder); });
        }
    }

    fs::path TextureFolder() const { return ResolveTextureFolder(draft_, file_.path).lexically_normal(); }

    // The packs in the player's order (mods.texture_pack_order).
    std::vector<platform::TexturePack> OrderedTexturePacks() const
    {
        return platform::OrderTexturePacks(texture_packs_, platform::SplitPackNames(draft_.texture_pack_order));
    }

    void MoveTexturePack(std::vector<platform::TexturePack> packs, std::size_t from, std::size_t to)
    {
        if (from >= packs.size() || to >= packs.size() || from == to) return;
        auto pack = std::move(packs[from]);
        packs.erase(packs.begin() + std::ptrdiff_t(from));
        packs.insert(packs.begin() + std::ptrdiff_t(to), std::move(pack));
        std::vector<std::string> names;
        for (const auto& item : packs) names.push_back(item.name);
        draft_.texture_pack_order = platform::JoinPackNames(names);
        dirty_ = true;
    }

    void SetTexturePackOn(const std::string& name, bool on)
    {
        auto off = platform::SplitPackNames(draft_.texture_packs_off);
        off.erase(std::remove(off.begin(), off.end(), name), off.end());
        if (!on) off.push_back(name);
        draft_.texture_packs_off = platform::JoinPackNames(off);
        dirty_ = true;
    }

    // A folder or .zip file dropped onto the Gameplay page: a pack is copied
    // into the texture packs folder in the background. False when it is none.
    bool AddTexturePack(const fs::path& source)
    {
        const auto pack = platform::ProbeTexturePack(source);
        if (!pack) return false;
        const fs::path folder = TextureFolder();
        std::error_code error;
        if (fs::equivalent(source.parent_path(), folder, error))
            Notice(pack->name + " is already in your texture packs folder.", NoticeKind::Info);
        else if (pack_copy_.valid())
            Notice("Wait until " + pack_copy_name_ + " is added.", NoticeKind::Warning);
        else if (fs::exists(folder / source.filename(), error))
            Notice("Your texture packs folder already has a " + pack->name + ".", NoticeKind::Warning);
        else
        {
            pack_copy_name_ = pack->name;
            pack_copy_textures_ = pack->textures;
            pack_copy_ = std::async(std::launch::async, [source, folder] { return CopyTexturePack(source, folder); });
        }
        return true;
    }

    // Copies under a hidden name first, so the folder never lists half a pack.
    static std::string CopyTexturePack(const fs::path& source, const fs::path& folder)
    {
        std::error_code error;
        fs::create_directories(folder, error);
        if (error) return "Cannot create " + PathUtf8(folder) + ": " + error.message();
        const auto name = source.filename();
        const auto temporary = folder / PathFromUtf8("." + PathUtf8(name) + ".adding");
        fs::remove_all(temporary, error);
        error.clear();
        if (fs::is_directory(source, error))
            fs::copy(source, temporary, fs::copy_options::recursive, error);
        else if (!error)
            fs::copy_file(source, temporary, error);
        if (!error) fs::rename(temporary, folder / name, error);
        if (!error) return {};
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        return "Cannot add " + PathUtf8(name) + ": " + error.message();
    }

    void ModsCard()
    {
        const auto& fonts = CurrentFonts();
        PollTexturePacks();
        BeginCard("##mods", "Mods", "Texture packs replace the game's textures with sharper ones. They change only "
                                    "how the game looks.", Icon::Sliders);
        if (BeginRows("##mods_rows"))
        {
            Row("Texture packs", "Texture packs made for Dolphin, from the folder below. Drop a pack's folder or .zip "
                                 "file onto this page to add it.");
            if (Toggle("##texture_packs", &draft_.texture_packs)) dirty_ = true;
            EndRows();
        }
        ImGui::BeginDisabled(!draft_.texture_packs);
        TextureFolderPicker();
        ImGui::Dummy({0, Dp(4)});
        if (!pack_copy_name_.empty()) PackCopyLine();
        const auto packs = OrderedTexturePacks();
        if (!packs.empty())
        {
            TexturePackTable(packs);
            if (packs.size() > 1)
                TextWrappedColored(fonts.caption, color::dim,
                                   "Drag a pack to change the order: where two packs replace the same texture, the "
                                   "higher one wins.");
        }
        else if (pack_copy_name_.empty())
            PackDropZone();
        ImGui::EndDisabled();

        ImGui::Dummy({0, Dp(6)});
        if (BeginRows("##dump_rows"))
        {
            Row("Dump textures", "For pack makers: each texture no pack replaces is saved to data/texture_dumps, "
                                 "named as texture packs need it. Takes disk space and can make the game stutter.");
            if (Toggle("##dump_textures", &draft_.dump_textures)) dirty_ = true;
            const fs::path dumps = DataFolder(base_) / "texture_dumps";
            std::error_code error;
            ImGui::SameLine(0, Dp(12));
            if (GhostButton("open_dumps", "Open", {GhostWidth("Open", Icon::Folder), Dp(30)},
                            fs::is_directory(dumps, error), Icon::Folder))
                OpenPath(dumps);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", fs::is_directory(dumps, error) ? PathUtf8(dumps).c_str()
                                                                       : "Made when the game first dumps a texture.");
            EndRows();
        }
        ImGui::Dummy({0, Dp(2)});
        TextWrappedColored(fonts.caption, color::muted,
            "Tweaks and texture packs apply when the game starts. For a bug report, try again with the tweaks off "
            "(or start the game with --no-tweaks) and without texture packs to check whether one of them is "
            "involved.");
        EndCard();
    }

    // The texture packs folder as one picker, like the game folder's: click it
    // (or Change...) to select another; Open shows it (and makes it).
    void TextureFolderPicker()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = Dp(60);
        const ImVec2 max{pos.x + width, pos.y + height};
        const float center = pos.y + height * 0.5f;
        bool browsing;
        { std::lock_guard<std::mutex> lock(folder_dialog_->mutex); browsing = folder_dialog_->active; }
        const bool can_select = !browsing && config_ok_ && draft_.texture_packs;

        const char* change = "Change...";
        const char* open = "Open";
        const float change_w = TextSize(fonts.body, change).x + Dp(32);
        const float open_w = GhostWidth(open, Icon::Folder);
        const float change_x = max.x - Dp(12) - change_w;
        const float open_x = change_x - Dp(8) - open_w;
        const float refresh_x = open_x - Dp(4) - Dp(34);

        ImGui::SetCursorScreenPos(pos);
        ImGui::BeginDisabled(!can_select);
        const bool picked = ImGui::InvisibleButton("##texture_folder", {refresh_x - Dp(10) - pos.x, height});
        ImGui::EndDisabled();
        const float hover = Animate(ImGui::GetID("##texture_folder_hover"), ImGui::IsItemHovered() && can_select, 12.0f);
        draw->AddRectFilled(pos, max, Col(Mix(Mix(color::field, color::card, 0.45f), color::field, hover)), Dp(12));
        draw->AddRect(pos, max, Col(Mix(color::border, color::accent, 0.45f * hover)), Dp(12), 0, Dp(1));
        const ImVec2 box{pos.x + Dp(12), center - Dp(18)};
        draw->AddRectFilled(box, {box.x + Dp(36), box.y + Dp(36)}, Col(color::accent, 0.12f + 0.06f * hover), Dp(10));
        DrawIcon(draw, Icon::Folder, {box.x + Dp(18), box.y + Dp(18)}, Dp(18), Col(color::accent));

        const fs::path path = TextureFolder();
        std::error_code error;
        std::string status;
        ImVec4 tint = color::muted;
        if (texture_scanning_ && texture_packs_.empty())
        {
            status = "Looking for texture packs...";
            tint = color::info;
        }
        else if (!fs::is_directory(path, error))
            status = "Not there yet: Open makes it, and a dropped pack goes into it";
        else
        {
            const auto off = platform::SplitPackNames(draft_.texture_packs_off);
            std::size_t used = 0, textures = 0;
            for (const auto& pack : texture_packs_)
                if (pack.error.empty() && std::find(off.begin(), off.end(), pack.name) == off.end())
                {
                    ++used;
                    textures += pack.textures;
                }
            const auto count = texture_packs_.size();
            status = count == 0 ? "No texture packs"
                : std::to_string(count) + (count == 1 ? " pack" : " packs") + ", " + std::to_string(used) + " in use ("
                    + std::to_string(textures) + " textures)";
        }
        const float text_x = box.x + Dp(36 + 14), text_w = std::max(Dp(80), refresh_x - Dp(14) - text_x);
        const float lines = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
        const float top = center - lines * 0.5f;
        DrawPath(draw, fonts.label, {text_x, top}, text_w, color::text, PathUtf8(path));
        DrawClipped(draw, fonts.caption, {text_x, top + FontSize(fonts.label) + Dp(3)}, text_w, tint, status);
        if (picked) BrowseFolder(FolderPurpose::TextureFolder, path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nClick to select another texture packs folder.",
                                                      PathUtf8(path).c_str());

        ImGui::SetCursorScreenPos({refresh_x, center - Dp(17)});
        if (GhostButton("refresh_texture_packs", "", Dp(34, 34), config_ok_ && !texture_scanning_, Icon::Refresh))
            texture_rescan_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for texture packs again.");
        ImGui::SetCursorScreenPos({open_x, center - Dp(17)});
        if (GhostButton("open_texture_folder", open, {open_w, Dp(34)}, config_ok_, Icon::Folder))
        {
            fs::create_directories(path, error);
            if (error) Notice("Cannot create " + PathUtf8(path) + ": " + error.message(), NoticeKind::Error);
            else
            {
                OpenPath(path);
                texture_rescan_ = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the folder (it is made when missing).");
        ImGui::SetCursorScreenPos({change_x, center - Dp(17)});
        if (SecondaryButton("change_texture_folder", change, {change_w, Dp(34)}, can_select))
            BrowseFolder(FolderPurpose::TextureFolder, path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select another texture packs folder.");
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({width, height});
    }

    // A pack being copied in (dropped onto the page).
    void PackCopyLine()
    {
        const auto& fonts = CurrentFonts();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float line = Dp(44);
        Spinner({at.x + Dp(22), at.y + line * 0.5f}, Dp(7), Col(color::info));
        const std::string text = "Adding " + pack_copy_name_ + " (" + std::to_string(pack_copy_textures_) + " textures)...";
        DrawLabel(ImGui::GetWindowDrawList(), fonts.body, {at.x + Dp(44), at.y + (line - FontSize(fonts.body)) * 0.5f},
                  Col(color::muted), text.c_str());
        ImGui::Dummy({ImGui::GetContentRegionAvail().x, line});
    }

    // No packs yet: a dashed area that says where they come from.
    void PackDropZone()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = Dp(104);
        const ImVec2 max{pos.x + width, pos.y + height};
        draw->AddRectFilled(pos, max, Col(color::field, 0.35f), Dp(14));
        // Dashes along the edges, drawn as small rectangles (the software
        // renderer garbles long thin lines).
        const float dash = Dp(8), gap = Dp(6), thick = Dp(1.2f), inset = Dp(14);
        const ImU32 ink = Col(color::border, 1.0f);
        for (float x = pos.x + inset; x + dash <= max.x - inset; x += dash + gap)
        {
            draw->AddRectFilled({x, pos.y}, {x + dash, pos.y + thick}, ink);
            draw->AddRectFilled({x, max.y - thick}, {x + dash, max.y}, ink);
        }
        for (float y = pos.y + inset; y + dash <= max.y - inset; y += dash + gap)
        {
            draw->AddRectFilled({pos.x, y}, {pos.x + thick, y + dash}, ink);
            draw->AddRectFilled({max.x - thick, y}, {max.x, y + dash}, ink);
        }
        const float cx = pos.x + width * 0.5f;
        draw->AddCircleFilled({cx, pos.y + Dp(30)}, Dp(17), Col(color::accent, 0.12f));
        DrawIcon(draw, Icon::Download, {cx, pos.y + Dp(30)}, Dp(18), Col(color::accent));
        const char* title = "Drop a texture pack here";
        const char* detail = "A pack's folder (such as R4Q) or its .zip file: it is copied into the folder above.";
        DrawLabel(draw, fonts.label, {cx - TextSize(fonts.label, title).x * 0.5f, pos.y + Dp(54)}, Col(color::text), title);
        DrawLabel(draw, fonts.caption, {cx - TextSize(fonts.caption, detail).x * 0.5f, pos.y + Dp(54) + FontSize(fonts.label) + Dp(4)},
                  Col(color::muted), detail);
        ImGui::Dummy({width, height});
    }

    void TexturePackTable(const std::vector<platform::TexturePack>& packs)
    {
        const auto& fonts = CurrentFonts();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(7, 0));
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4{0, 0, 0, 0});
        if (ImGui::BeginTable("##texture_packs_table", 5,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoClip))
        {
            const auto fixed = ImGuiTableColumnFlags_WidthFixed;
            ImGui::TableSetupColumn("ORDER", fixed, Dp(58));
            ImGui::TableSetupColumn("PACK", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("TEXTURES", fixed, Dp(84));
            ImGui::TableSetupColumn("IN USE", fixed, Dp(64));
            ImGui::TableSetupColumn("##menu", fixed, Dp(34));
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers, Dp(24));
            ImGui::PushFont(fonts.caption);
            ImGui::PushStyleColor(ImGuiCol_Text, color::dim);
            for (int column = 0; column < ImGui::TableGetColumnCount(); ++column)
            {
                ImGui::TableSetColumnIndex(column);
                ImGui::TableHeader(ImGui::TableGetColumnName(column));
            }
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (const ImGuiTable* table = ImGui::GetCurrentTable())
            {
                const float y = ImGui::GetItemRectMax().y + Dp(3);
                ImGui::GetWindowDrawList()->AddRectFilled({table->WorkRect.Min.x, y}, {table->WorkRect.Max.x, y + Dp(1)},
                                                          Col(color::border));
            }
            const auto off = platform::SplitPackNames(draft_.texture_packs_off);
            for (std::size_t n = 0; n < packs.size(); ++n) TexturePackRow(packs, n, off);
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    void TexturePackRow(const std::vector<platform::TexturePack>& packs, std::size_t n,
                        const std::vector<std::string>& off)
    {
        const auto& fonts = CurrentFonts();
        const auto& pack = packs[n];
        const bool storable = platform::StorablePackName(pack.name);
        const bool readable = pack.error.empty();
        const bool reorder = storable && packs.size() > 1;
        bool on = readable && std::find(off.begin(), off.end(), pack.name) == off.end();
        const float height = Dp(58);
        ImGui::PushID(pack.name.c_str());
        ImGui::TableNextRow(ImGuiTableRowFlags_None, height);
        ImGui::TableSetColumnIndex(0);
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                          {0, height});
        const bool hovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        const ImGuiTable* table = ImGui::GetCurrentTable();
        const float left = table ? table->WorkRect.Min.x : cell.x;
        const float right = table ? table->WorkRect.Max.x : cell.x + ImGui::GetContentRegionAvail().x;
        const ImVec2 box_min{left, cell.y + Dp(3)}, box_max{right, cell.y + height - Dp(3)};
        constexpr const char* kPayload = "RECHARGED_TEXTURE_PACK";
        const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        const bool moving = dragging && dragging->IsDataType(kPayload) && *static_cast<const int*>(dragging->Data) == int(n);
        if (reorder && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
        {
            const int index = int(n);
            ImGui::SetDragDropPayload(kPayload, &index, sizeof index);
            ImGui::EndDragDropSource();
        }
        if (reorder && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    kPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                const std::size_t from = std::size_t(*static_cast<const int*>(payload->Data));
                if (from != n)
                {
                    // Where it lands: above this row moving up, below it moving down.
                    const float y = from > n ? box_min.y - Dp(2) : box_max.y + Dp(1);
                    draw->AddRectFilled({left + Dp(6), y}, {right - Dp(6), y + Dp(2)}, Col(color::accent), Dp(1));
                }
                if (payload->IsDelivery()) MoveTexturePack(packs, from, n);
            }
            ImGui::EndDragDropTarget();
        }
        const float hover = Animate(ImGui::GetID("##hover"), hovered || moving, 14.0f);
        if (moving)
            draw->AddRect(box_min, box_max, Col(color::accent, 0.6f), Dp(12), 0, Dp(1.2f));
        if (hover > 0.0f) draw->AddRectFilled(box_min, box_max, Col(color::text, 0.05f * hover), Dp(12));
        if (hovered && !dragging)
            ImGui::SetTooltip("%s%s", PathUtf8(pack.path).c_str(), reorder ? "\nDrag to change the order." : "");
        const float fade = on ? 1.0f : 0.55f;
        const float center = cell.y + height * 0.5f;

        // Order: a grip to drag it by, and its place.
        const ImU32 grip = Col(color::dim, hovered && reorder ? 1.0f : 0.6f);
        for (int row = -1; row <= 1; ++row)
            for (int column = 0; column < 2; ++column)
                draw->AddCircleFilled({cell.x + Dp(6 + 5.0f * float(column)), center + Dp(5.0f * float(row))}, Dp(1.5f),
                                      grip);
        const ImVec2 badge{cell.x + Dp(34), center};
        draw->AddCircleFilled(badge, Dp(12), Col(n == 0 && on ? color::accent : color::border, n == 0 && on ? 0.18f : 0.7f));
        const std::string place = std::to_string(n + 1);
        const ImVec2 place_size = TextSize(fonts.caption, place.c_str());
        DrawLabel(draw, fonts.caption, {badge.x - place_size.x * 0.5f, badge.y - place_size.y * 0.5f},
                  Col(n == 0 && on ? color::accent : color::text), place.c_str());

        // The pack: what it is, its name and its state.
        ImGui::TableSetColumnIndex(1);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float column = ImGui::GetContentRegionAvail().x;
        const ImVec4 tint = readable ? color::accent : color::danger;
        const ImVec2 icon{at.x, center - Dp(17)};
        draw->AddRectFilled(icon, {icon.x + Dp(34), icon.y + Dp(34)}, Col(tint, 0.12f * fade), Dp(9));
        DrawIcon(draw, pack.archive ? Icon::Archive : Icon::Folder, {icon.x + Dp(17), icon.y + Dp(17)}, Dp(17),
                 Col(tint, fade));
        const float text_x = at.x + Dp(46), text_w = column - Dp(48);
        const float lines = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
        const float top = center - lines * 0.5f;
        DrawClipped(draw, fonts.label, {text_x, top}, text_w, Mix(color::muted, color::text, fade), pack.name);
        std::string detail = pack.archive ? "Zip file" : "Folder";
        ImVec4 detail_tint = color::muted;
        if (!readable)
        {
            detail = "Cannot be read: " + pack.error;
            detail_tint = color::danger;
        }
        else if (!on)
            detail += ", not in use";
        DrawClipped(draw, fonts.caption, {text_x, top + FontSize(fonts.label) + Dp(3)}, text_w, detail_tint, detail);

        // Textures, and whether it is in use.
        ImGui::TableSetColumnIndex(2);
        const std::string textures = std::to_string(pack.textures);
        DrawLabel(draw, fonts.body, {ImGui::GetCursorScreenPos().x, center - FontSize(fonts.body) * 0.5f},
                  Col(Mix(color::dim, color::text, fade)), textures.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, center - Dp(14)});
        ImGui::BeginDisabled(!storable || !readable);
        if (Toggle("##on", &on)) SetTexturePackOn(pack.name, on);
        ImGui::EndDisabled();

        // Its menu: the same moves without dragging (keyboard, gamepad), and its place on disk.
        ImGui::TableSetColumnIndex(4);
        ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, center - Dp(16)});
        if (GhostButton("menu", "", Dp(32, 32), true, Icon::More)) ImGui::OpenPopup("pack_menu");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("More");
        const float width = Dp(220);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, Dp(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(10));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(color::card, color::window, 0.25f));
        if (ImGui::BeginPopup("pack_menu"))
        {
            ImGui::BeginDisabled(!reorder || n == 0);
            if (MenuAction("up", "Move up", width)) MoveTexturePack(packs, n, n - 1);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!reorder || n + 1 >= packs.size());
            if (MenuAction("down", "Move down", width)) MoveTexturePack(packs, n, n + 1);
            ImGui::EndDisabled();
            MenuSeparator(width);
            if (MenuAction("show", "Show in folder", width)) OpenPath(pack.path.parent_path());
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        ImGui::PopID();
    }

    void AdvancedPage()
    {
        PageHeader("Advanced", "Troubleshooting and development options. The defaults suit most players.");
        BeginCard("##interface", "Interface", nullptr, Icon::Sliders);
        if (BeginRows("##interface_rows"))
        {
            Row("Launcher size", "Scales this launcher. Automatic follows your display's scaling setting.");
            const char* values[] = {"auto", "75", "100", "125", "150", "175", "200"};
            const char* names[] = {"Automatic", "75%", "100%", "125%", "150%", "175%", "200%"};
            const char* preview = draft_.ui_scale.c_str();
            for (int i = 0; i < 7; ++i) if (draft_.ui_scale == values[i]) preview = names[i];
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##ui_scale", preview))
            {
                for (int i = 0; i < 7; ++i)
                    if (ImGui::Selectable(names[i], draft_.ui_scale == values[i])) { draft_.ui_scale = values[i]; dirty_ = true; }
                ImGui::EndCombo();
            }
            Row("Reopen after the game", "Opens the launcher again when you close the game.");
            if (Toggle("##reopen_after_game", &draft_.reopen_after_game)) dirty_ = true;
            EndRows();
        }
        EndCard();

        BeginCard("##updates", "Updates", nullptr, Icon::Download);
        if (BeginRows("##update_rows"))
        {
            Row("Check for updates", "Looks for a newer release, or test build, each time the launcher starts. "
                "The Play page shows what it finds and switches between releases and test builds.");
            if (Toggle("##check_updates", &draft_.check_updates)) dirty_ = true;
            Row("Status", UpdateSummary().c_str());
            ImGui::BeginDisabled(!updates_ || !config_ok_ || UpdateBusy());
            if (SecondaryButton("##check_now", "Check now", Dp(150, 38), true, Icon::Refresh))
                CheckForUpdates(UpdatePrompt::Any, UpdateChannelSetting());
            ImGui::EndDisabled();
            EndRows();
        }
        EndCard();

        BeginCard("##graphics", "Graphics", nullptr, Icon::Display);
        if (BeginRows("##graphics_rows"))
        {
#if defined(_WIN32)
            // Automatic is its own choice: only it may switch to DirectX 12.
            Row("Graphics API", "Automatic uses Vulkan, or DirectX 12 when Vulkan cannot start. Used the next time you start the game.");
            const char* preview = draft_.backend == "auto" ? "Automatic" : "Choose a graphics API";
            for (const auto& choice : kGraphicsBackends)
                if (draft_.backend == choice.value) preview = choice.label;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##graphics_api", preview))
            {
                if (ImGui::Selectable("Automatic", draft_.backend == "auto"))
                { draft_.backend = "auto"; dirty_ = true; }
                for (const auto& choice : kGraphicsBackends)
                    if (ImGui::Selectable(choice.label, draft_.backend == choice.value))
                    { draft_.backend = choice.value; dirty_ = true; }
                ImGui::EndCombo();
            }
#else
            Row("Graphics API", "Used the next time you start the game.");
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(4));
            Chip(kGraphicsBackends.front().label, color::info);
#endif
#if !defined(__APPLE__)
            GraphicsCardRow();
#endif
            EndRows();
        }
        EndCard();

        BeginCard("##diagnostics", "Diagnostics & analytics", "Local tools for investigating performance and graphics issues.", Icon::Info);
        if (BeginRows("##diagnostic_rows"))
        {
            Row("Performance stats", "Records frame times for each game session, with the arena and settings, and a summary per arena. Saves a CSV and summary in the logs folder; testers can share them.");
            if (Toggle("##frame_logging", &draft_.frame_logging)) dirty_ = true;

            const auto recordings = base_ / "logs";
            const auto recordingsFolder = PathUtf8(recordings);
            Row("Frame recordings", recordingsFolder.c_str());
            if (SecondaryButton("open_frame_recordings", "Open folder", {ImGui::GetContentRegionAvail().x, Dp(40)},
                                true, Icon::Folder))
            {
                std::error_code error;
                fs::create_directories(recordings, error);
                OpenPath(recordings);
            }

            Row("Shader cache", "Clears compiled shaders for all graphics APIs. The next launch prepares them again.");
            if (SecondaryButton("clear_shader_cache", "Clear shader cache", {ImGui::GetContentRegionAvail().x, Dp(40)},
                                true, Icon::Refresh))
            {
                try
                {
                    const auto cleared = platform::ClearShaderCache(platform::ShaderCacheDirectory(base_));
                    Notice(cleared.removed_files ? "Shader cache cleared. Graphics will be prepared again on the next launch."
                                                : "The shader cache is already empty.", NoticeKind::Success);
                }
                catch (const std::exception& error) { Notice(error.what(), NoticeKind::Error); }
            }

            Row("API validation", "Checks every graphics call for errors. Useful for bug reports, but slower.");
            if (Toggle("##validation", &draft_.graphics_validation)) dirty_ = true;

            Row("Log detail", "Which messages reach the console: errors, warnings, info or debug.");
            const char* values[] = {"error", "warning", "info", "debug"};
            int level = 2;
            for (int i = 0; i < 4; ++i) if (draft_.log_level == values[i]) level = i;
            if (Segmented("##log_level", &level, {"Errors", "Warnings", "Info", "Debug"}, ImGui::GetContentRegionAvail().x))
            { draft_.log_level = values[level]; dirty_ = true; }

            Row("Verbose console", "Also prints development traces and the game's own debug output. Useful for bug reports.");
            if (Toggle("##verbose_console", &draft_.verbose_console)) dirty_ = true;
#if defined(_WIN32) || defined(__APPLE__)
            Row("Log window", "Shows the log in a separate window while Recharged runs, for testing. Closing it doesn't stop the game.");
            if (Toggle("##log_window", &draft_.log_window)) dirty_ = true;
#endif

            EndRows();
        }
        EndCard();

        BeginCard("##configuration", "Configuration", nullptr, Icon::Sliders);
        if (BeginRows("##configuration_rows"))
        {
            const std::string settings_path = PathUtf8(file_.path);
            Row("Settings file", settings_path.c_str());
            if (SecondaryButton("open_settings", "Open folder", {ImGui::GetContentRegionAvail().x, Dp(40)},
                                fs::exists(file_.path.parent_path()), Icon::Folder))
                OpenPath(file_.path.parent_path());

            Row("Launch command", "Copies a command line that starts the game with these settings.");
            if (SecondaryButton("copy_command", "Copy command", {ImGui::GetContentRegionAvail().x, Dp(40)}, true, Icon::Copy))
            {
                const auto command = LaunchCommand();
                if (SDL_SetClipboardText(command.c_str())) Notice("Launch command copied to the clipboard.", NoticeKind::Success);
                else Notice(std::string("Cannot copy: ") + SDL_GetError(), NoticeKind::Error);
            }
            EndRows();
        }
        EndCard();

        BeginCard("##reset", "Reset", nullptr, Icon::Refresh);
        if (BeginRows("##reset_rows"))
        {
            Row("Restore defaults", "Resets every setting except your game image and game folder. Save to keep the result.");
            if (SecondaryButton("defaults", "Restore defaults", {ImGui::GetContentRegionAvail().x, Dp(40)}, config_ok_, Icon::Refresh))
            {
                const auto disc = draft_.disc;
                const auto folder = draft_.folder;
                const bool subfolders = draft_.folder_subfolders;
                draft_ = {};
                draft_.disc = disc;
                draft_.folder = folder;
                draft_.folder_subfolders = subfolders;
                dirty_ = true;
                Notice("Defaults restored. Save to keep them.", NoticeKind::Info);
            }
            EndRows();
        }
        EndCard();
    }

    void AboutPage()
    {
        AboutBanner();
        AboutDevelopers();
        AboutThanks();
        AboutCommunity();
        AboutFoundations();
        AboutBuild();
    }

    // The page's banner: the name and what it is (the version is in the sidebar).
    void AboutBanner()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float pad = Dp(30);
        const float wordmark = Dp(50); // the hero font, drawn smaller
        const float height = pad + FontSize(fonts.caption) + Dp(6) + wordmark + Dp(6) + FontSize(fonts.heading) + pad;
        const ImVec2 max{pos.x + width, pos.y + height};
        const float rounding = Dp(16);

        // A dark green wash and, on the right, a lightning bolt in a soft glow.
        FillGradient(draw, pos, max, Col(Mix(color::card, color::accent_dark, 0.9f)), Col(color::card), rounding, true);
        draw->PushClipRect(pos, max, true);
        const ImVec2 glow{max.x - std::min(Dp(140), width * 0.2f), pos.y + height * 0.5f};
        for (int i = 24; i >= 1; --i)
            draw->AddCircleFilled(glow, height * 0.035f * float(i), Col(color::accent, 0.007f), 64);
        if (width >= Dp(520))
        {
            const float s = height * 0.72f;
            auto at = [&](float x, float y) { return ImVec2{glow.x + x * s, glow.y + y * s}; };
            const ImVec2 bolt[] = {at(-0.05f, -0.50f), at(0.25f, -0.50f), at(0.06f, -0.10f), at(0.30f, -0.10f),
                                   at(-0.18f, 0.50f),  at(-0.02f, 0.04f), at(-0.27f, 0.04f)};
            draw->AddConcavePolyFilled(bolt, 7, Col(color::accent, 0.08f));
            draw->AddPolyline(bolt, 7, Col(color::accent_hi, 0.30f), ImDrawFlags_Closed, Dp(1.5f));
        }
        draw->PopClipRect();
        draw->AddRect(pos, max, Col(color::border), rounding, 0, Dp(1));
        // A highlight along the top edge, fading out to both sides.
        const float middle = pos.x + width * 0.5f;
        const ImU32 clear = Col(color::accent_hi, 0.0f), bright = Col(color::accent_hi, 0.45f);
        draw->AddRectFilledMultiColor({pos.x + width * 0.12f, pos.y}, {middle, pos.y + Dp(1)}, clear, bright, bright, clear);
        draw->AddRectFilledMultiColor({middle, pos.y}, {max.x - width * 0.12f, pos.y + Dp(1)}, bright, clear, clear, bright);

        float y = pos.y + pad;
        const float x = pos.x + pad;
        DrawTracked(draw, fonts.caption, {x, y}, Col(color::accent, 0.95f), "ABOUT", Dp(2.6f));
        y += FontSize(fonts.caption) + Dp(6);
        // "RECHARGED" as on the Play page: a soft glow and a lime-to-green gradient.
        for (int i = 3; i >= 1; --i)
            draw->AddText(fonts.hero, wordmark, {x, y + Dp(float(i))}, Col(color::accent, 0.07f * float(i)), "RECHARGED");
        const int start = draw->VtxBuffer.Size;
        draw->AddText(fonts.hero, wordmark, {x, y}, IM_COL32_WHITE, "RECHARGED");
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, {x, y}, {x, y + wordmark},
                                                     Col(color::accent_hi), Col(color::accent));
        y += wordmark + Dp(6);
        const char* game = "Mario Strikers Charged";
        DrawLabel(draw, fonts.heading, {x, y}, Col(color::text), game);
        DrawLabel(draw, fonts.heading, {x + TextSize(fonts.heading, game).x, y}, Col(color::text, 0.72f),
                  ", natively on your PC");

        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({width, height});
        ImGui::Dummy({0, Dp(8)});
    }

    // A small tracked heading inside a card.
    void AboutOverline(const char* text)
    {
        DrawTracked(ImGui::GetWindowDrawList(), CurrentFonts().caption, ImGui::GetCursorScreenPos(),
                    Col(color::accent, 0.95f), text, Dp(1.8f));
        ImGui::Dummy({0, FontSize(CurrentFonts().caption) + Dp(4)});
    }

    void AboutDivider()
    {
        ImGui::Dummy({0, Dp(10)});
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        // A filled rectangle: thin lines break up in the software renderer.
        ImGui::GetWindowDrawList()->AddRectFilled(pos, {pos.x + ImGui::GetContentRegionAvail().x, pos.y + Dp(1)},
                                                  Col(color::border, 0.8f));
        ImGui::Dummy({0, Dp(14)});
    }

    // Lays out buttons left to right, moving to the next line when one does not fit.
    void FlowNext(bool first, float width)
    {
        const float gap = Dp(10);
        if (!first && ImGui::GetItemRectMax().x + gap + width <= ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x)
            ImGui::SameLine(0, gap);
    }

    struct Person
    {
        const char* name;
        const char* line; // under the name: what they do
        ImVec4 tint;
    };

    // A person: a round badge with the name's first letter, the name and a line under it.
    void PersonTile(const Person& person, float width)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec4& tint = person.tint;
        const char letter[] = {char(std::toupper(static_cast<unsigned char>(person.name[0]))), '\0'};
        const float badge = Dp(46);
        const float text_x = Dp(18) + badge + Dp(14);
        const float wrap = width - text_x - Dp(16);
        const float line_height = fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, wrap, person.line).y;
        const float height = std::max(badge + Dp(28), FontSize(fonts.label) + Dp(2) + line_height + Dp(28));
        const ImVec2 max{pos.x + width, pos.y + height};
        draw->AddRectFilled(pos, max, Col(color::field), Dp(14));
        draw->AddRect(pos, max, Col(tint, 0.28f), Dp(14), 0, Dp(1));
        const ImVec2 center{pos.x + Dp(18) + badge * 0.5f, pos.y + height * 0.5f};
        draw->AddCircleFilled(center, badge * 0.5f + Dp(4), Col(tint, 0.12f), 40);
        const int start = draw->VtxBuffer.Size;
        draw->AddCircleFilled(center, badge * 0.5f, IM_COL32_WHITE, 40);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size,
                                                     {center.x, center.y - badge * 0.5f},
                                                     {center.x, center.y + badge * 0.5f},
                                                     Col(Mix(tint, color::text, 0.35f)), Col(tint));
        const ImVec2 size = TextSize(fonts.title, letter);
        DrawLabel(draw, fonts.title, {center.x - size.x * 0.5f, center.y - size.y * 0.5f},
                  Col(Mix(tint, color::window, 0.85f)), letter);
        const float text_y = pos.y + (height - (FontSize(fonts.label) + Dp(2) + line_height)) * 0.5f;
        DrawLabel(draw, fonts.label, {pos.x + text_x, text_y}, Col(color::text), person.name);
        // Drawn here, wrapped in the tile (TextWrappedColored wraps at the window's edge).
        draw->AddText(fonts.caption, FontSize(fonts.caption), {pos.x + text_x, text_y + FontSize(fonts.label) + Dp(2)},
                      Col(color::muted), person.line, nullptr, wrap);
        ImGui::Dummy({width, height});
    }

    // Two people side by side when they fit, else one under the other.
    void PeopleTiles(const Person& first, const Person& second)
    {
        const float gap = Dp(12);
        const float available = ImGui::GetContentRegionAvail().x;
        const bool side_by_side = available >= Dp(460);
        const float width = side_by_side ? (available - gap) * 0.5f : available;
        PersonTile(first, width);
        if (side_by_side) ImGui::SameLine(0, gap);
        else ImGui::Dummy({0, gap - ImGui::GetStyle().ItemSpacing.y});
        PersonTile(second, width);
    }

    // Names as pills, flowing over as many lines as they need. With `links`
    // each opens the person's GitHub page.
    void NamePills(const char* id, const char* const* names, std::size_t count, bool links)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        ImGui::PushID(id);
        const float left = ImGui::GetCursorScreenPos().x;
        const float right = left + ImGui::GetContentRegionAvail().x;
        const float gap = Dp(8);
        const float height = Dp(32);
        ImVec2 at = ImGui::GetCursorScreenPos();
        for (std::size_t i = 0; i < count; ++i)
        {
            const ImVec2 label = TextSize(fonts.body, names[i]);
            const float icon = links ? Dp(12) + Dp(8) : 0.0f;
            const ImVec2 size{Dp(16) + label.x + icon + Dp(16), height};
            if (i && at.x + size.x > right)
                at = {left, at.y + height + gap};
            ImGui::SetCursorScreenPos(at);
            ImGui::PushID(int(i));
            float hover = 0.0f;
            if (links)
            {
                const bool pressed = ImGui::InvisibleButton("##pill", size);
                hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 12.0f);
                const std::string url = std::string("https://github.com/") + names[i];
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", url.c_str());
                if (pressed) SDL_OpenURL(url.c_str());
            }
            else ImGui::Dummy(size);
            ImGui::PopID();
            const ImVec2 max{at.x + size.x, at.y + size.y};
            draw->AddRectFilled(at, max, Col(Mix(color::field, color::border, 0.5f * hover)), height * 0.5f);
            draw->AddRect(at, max, Col(Mix(color::border, color::accent, 0.5f * hover)), height * 0.5f, 0, Dp(1));
            const ImU32 ink = Col(Mix(color::text, color::accent_hi, 0.4f * hover));
            DrawLabel(draw, fonts.body, {at.x + Dp(16), at.y + (height - label.y) * 0.5f}, ink, names[i]);
            if (links)
                DrawIcon(draw, Icon::Link, {max.x - Dp(16) - Dp(6), at.y + height * 0.5f}, Dp(12),
                         Col(Mix(color::muted, color::accent_hi, hover)));
            at.x += size.x + gap;
        }
        ImGui::SetCursorScreenPos({left, at.y + height});
        ImGui::Dummy({0, 0});
        ImGui::PopID();
    }

    // Who writes Recharged, as the commits show. tools/update_issue_reporters.py
    // leaves the developers and the special thanks out of the issue reporters.
    void AboutDevelopers()
    {
        BeginCard("##developers", "Developers", "The people who build Recharged.", Icon::Code);
        PeopleTiles({"Yannick Suter", "Decompilation, native port and launcher", color::accent_hi},
                    {"GoldenPalazzo", "Networking and gameplay fixes", color::info});
        EndCard();
    }

    void AboutThanks()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##thanks", "Thank you", "Recharged is shaped by the people who play it.", Icon::Heart);

        AboutOverline("SPECIAL THANKS");
        TextWrappedColored(fonts.body, color::text, "For intense testing, build after build.");
        ImGui::Dummy({0, Dp(4)});
        PeopleTiles({"einBirnenbaum", "Tester", color::accent}, {"GoldCobra", "Tester", color::warning});

        AboutDivider();
        AboutOverline("ISSUE REPORTERS");
        TextWrappedColored(fonts.body, color::text,
            "Everyone who reported a problem or asked for a feature on GitHub. Every issue makes Recharged better: "
            "this is how we roll!");
        ImGui::Dummy({0, Dp(4)});
        NamePills("##reporters", kIssueReporters, std::size(kIssueReporters), false);
        ImGui::Dummy({0, Dp(6)});
        const BuildIdentity identity = updates_ ? updates_->Build() : CurrentBuild();
        const std::string issues = "https://github.com/" + identity.repository + "/issues/new";
        const ImVec2 button = Dp(200, 40);
        FlowNext(true, button.x);
        if (SecondaryButton("report_problem", "Report a problem", button, true, Icon::Bug))
            SDL_OpenURL((issues + "/choose").c_str());
        FlowNext(false, button.x);
        if (SecondaryButton("suggest_feature", "Suggest a feature", button, true, Icon::Bulb))
            SDL_OpenURL((issues + "?template=feature_request.yml").c_str());

        AboutDivider();
        AboutOverline("DECOMPILATION");
        TextWrappedColored(fonts.body, color::text,
            "To the contributors of the decompilation, who helped bring it over the last miles.");
        ImGui::Dummy({0, Dp(4)});
        static constexpr const char* contributors[] = {"Jasu14", "vZylev", "GoldenPalazzo"};
        NamePills("##contributors", contributors, std::size(contributors), true);
        EndCard();
    }

    // The Mario Strikers community's Discord server: what it is, a thank you
    // and the way in. Drawn like a card, with Discord's colour.
    void AboutCommunity()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const char* invite = "https://discord.gg/de2YaWg";
        const char* about = "The central hub of Mario Strikers players: tournaments, matchmaking and league play "
                            "across the series, with fans, competitors and creators from around the world.";
        const char* thanks = "Thank you for the matches, the feedback and all the enthusiasm!";
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float pad = Dp(22), badge = Dp(52), button_height = Dp(40);
        const float text_x = pos.x + pad + badge + Dp(18);
        const float wrap = std::max(Dp(120), pos.x + width - pad - text_x);
        const auto text_height = [&](const char* text) {
            return fonts.body->CalcTextSizeA(FontSize(fonts.body), FLT_MAX, wrap, text).y;
        };
        const float height = pad + FontSize(fonts.caption) + Dp(4) + FontSize(fonts.heading) + Dp(8)
            + text_height(about) + Dp(4) + text_height(thanks) + Dp(16) + button_height + pad;
        const ImVec2 max{pos.x + width, pos.y + height};
        const float rounding = Dp(16);
        FillGradient(draw, pos, max, Col(Mix(color::card, color::discord, 0.20f)), Col(color::card), rounding, true);
        draw->AddRect(pos, max, Col(Mix(color::border, color::discord, 0.45f)), rounding, 0, Dp(1));
        // The badge: Discord's logo on a round blurple badge with a halo.
        const ImVec2 center{pos.x + pad + badge * 0.5f, pos.y + pad + badge * 0.5f};
        draw->AddCircleFilled(center, badge * 0.5f + Dp(5), Col(color::discord, 0.16f), 48);
        const ImVec4 lighter = Mix(color::discord, color::text, 0.25f);
        const int start = draw->VtxBuffer.Size;
        draw->AddCircleFilled(center, badge * 0.5f, IM_COL32_WHITE, 48);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size,
                                                     {center.x, center.y - badge * 0.5f},
                                                     {center.x, center.y + badge * 0.5f}, Col(lighter),
                                                     Col(color::discord));
        DrawDiscordMark(draw, center, badge * 0.56f, IM_COL32_WHITE, Col(Mix(lighter, color::discord, 0.5f)));

        float y = pos.y + pad;
        DrawTracked(draw, fonts.caption, {text_x, y}, Col(Mix(color::discord, color::text, 0.45f)), "COMMUNITY", Dp(1.8f));
        y += FontSize(fonts.caption) + Dp(4);
        DrawLabel(draw, fonts.heading, {text_x, y}, Col(color::text), "Mario Strikers on Discord");
        y += FontSize(fonts.heading) + Dp(8);
        draw->AddText(fonts.body, FontSize(fonts.body), {text_x, y}, Col(color::muted), about, nullptr, wrap);
        y += text_height(about) + Dp(4);
        draw->AddText(fonts.body, FontSize(fonts.body), {text_x, y}, Col(color::text), thanks, nullptr, wrap);
        y += text_height(thanks) + Dp(16);
        ImGui::SetCursorScreenPos({text_x, y});
        if (DiscordButton("join_discord", "Join on Discord", {Dp(200), button_height})) SDL_OpenURL(invite);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", invite);

        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({width, height});
        ImGui::Dummy({0, Dp(8)});
    }

    // What Recharged is built on: the decompilation and Aurora, side by side when they fit.
    void AboutFoundations()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##foundations", "Built on", nullptr, Icon::Layers);
        const float title = Dp(30);
        auto decomp_heading = [&] {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(pos, {pos.x + title, pos.y + title}, Col(color::accent, 0.12f), Dp(8));
            DrawIcon(draw, Icon::Disc, {pos.x + title * 0.5f, pos.y + title * 0.5f}, Dp(16), Col(color::accent));
            DrawLabel(draw, fonts.label, {pos.x + title + Dp(12), pos.y + (title - FontSize(fonts.label)) * 0.5f},
                      Col(color::text), "The decompilation");
            ImGui::Dummy({title, title + Dp(4)});
        };
        auto aurora_heading = [&] {
            if (!aurora_logo_)
            {
                TextColored(fonts.label, color::text, "Aurora");
                return;
            }
            const float height = Dp(22);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({pos.x, pos.y + (title - height) * 0.5f});
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(aurora_logo_)),
                         {height * aurora_logo_width_ / aurora_logo_height_, height});
            ImGui::SetCursorScreenPos(pos);
            ImGui::Dummy({0, title + Dp(4)});
        };
        const char* decomp_text =
            "Recharged compiles the reconstructed C/C++ source of Mario Strikers Charged to native code. The "
            "original game logic runs unchanged; Recharged provides the platform underneath it: graphics, audio, "
            "input, files and timing.";
        const char* aurora_text =
            "Aurora by Luke Street provides the GameCube/Wii platform layer the game runs on: GX graphics on "
            "modern GPUs, plus windowing, input and system services.";
        auto text = [&](const char* body) {
            TextWrappedColored(fonts.body, color::muted, body);
            ImGui::Dummy({0, Dp(6)});
        };
        auto link = [&](const char* id, const char* label, const char* url) {
            if (SecondaryButton(id, label, {ImGui::GetContentRegionAvail().x, Dp(40)}, true, Icon::GitHub))
                SDL_OpenURL(url);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", url);
        };
        const char* decomp_url = "https://github.com/yannicksuter/mscharged-decomp";
        const char* aurora_url = "https://github.com/encounter/aurora";
        if (ImGui::GetContentRegionAvail().x < Dp(600))
        {
            decomp_heading();
            text(decomp_text);
            link("decomp_link", "Decompilation on GitHub", decomp_url);
            ImGui::Dummy({0, Dp(14)});
            aurora_heading();
            text(aurora_text);
            link("aurora_link", "Aurora on GitHub", aurora_url);
        }
        else
        {
            // Side by side, one table row per part, so the texts and the buttons line up.
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 0));
            if (ImGui::BeginTable("##foundation_table", 3, ImGuiTableFlags_None))
            {
                ImGui::TableSetupColumn("decomp", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("gap", ImGuiTableColumnFlags_WidthFixed, Dp(28));
                ImGui::TableSetupColumn("aurora", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                decomp_heading();
                ImGui::TableSetColumnIndex(2);
                aurora_heading();
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                text(decomp_text);
                ImGui::TableSetColumnIndex(2);
                text(aurora_text);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                link("decomp_link", "Decompilation on GitHub", decomp_url);
                ImGui::TableSetColumnIndex(2);
                link("aurora_link", "Aurora on GitHub", aurora_url);
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }
        AboutDivider();
        TextWrappedColored(fonts.caption, color::muted,
            "Unofficial project, not affiliated with Nintendo or Next Level Games. You need your own copy of the game.");
        ImGui::Dummy({0, Dp(2)});
        TextWrappedColored(fonts.caption, color::dim,
            "Recharged's original code, tools and documentation: CC0 1.0. Header artwork: SteamGridDB hero 8926 by "
            "Jiquita. Font: Roboto (Apache 2.0). Built with SDL3 (zlib), Dear ImGui (MIT), Aurora (MIT), Dawn (BSD), "
            "nod and its Rust dependencies, and other libraries listed in LICENSES. Each keeps its own license.");
        EndCard();
    }

    // Facts about this build in a grid, and what a bug report needs: a copy of
    // them and the log.
    void AboutBuild()
    {
        const auto& fonts = CurrentFonts();
        const BuildIdentity identity = updates_ ? updates_->Build() : CurrentBuild();
        std::vector<std::pair<const char*, std::string>> facts;
        facts.emplace_back("Version", build::version);
        facts.emplace_back("Origin", DescribeOrigin(identity));
        if (identity.built) facts.emplace_back("Built", LocalTime(identity.built));
        else if (identity.commit_time) facts.emplace_back("Commit time", LocalTime(identity.commit_time));
        facts.emplace_back("Commit", identity.commit.substr(0, 12));
        const int sdl = SDL_GetVersion();
        facts.emplace_back("Platform", std::string(SDL_GetPlatform()) + ", SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl))
            + "." + std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(sdl)));
        const char* renderer = SDL_GetRendererName(renderer_);
        char display[96];
        std::snprintf(display, sizeof display, "%s, interface x%.2f, pixel density x%.2f",
                      renderer ? renderer : "unknown renderer", metrics_.ui, metrics_.density);
        facts.emplace_back("Launcher display", display);
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
        facts.emplace_back("Game runtime", "Included");
#else
        facts.emplace_back("Game runtime", "Not included (launcher-only build)");
#endif
        facts.emplace_back("Configuration", MSCHARGED_BUILD_CONFIG);

        BeginCard("##build", "This build", "For a bug report: copy these details and attach the log.", Icon::Info);
        const int columns = ImGui::GetContentRegionAvail().x >= Dp(600) ? 3 : 2;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 7));
        if (ImGui::BeginTable("##build_facts", columns, ImGuiTableFlags_SizingStretchSame))
        {
            for (const auto& [label, value] : facts)
            {
                ImGui::TableNextColumn();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - Dp(16));
                TextColored(fonts.caption, color::dim, label);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(4));
                TextWrappedColored(fonts.body, color::text, value.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::Dummy({0, Dp(4)});

        const ImVec2 button = Dp(200, 40);
        FlowNext(true, button.x);
        if (SecondaryButton("copy_build", "Copy build details", button, true, Icon::Copy))
        {
            std::string text = "Recharged\n";
            for (const auto& [label, value] : facts) text += std::string(label) + ": " + value + "\n";
            if (SDL_SetClipboardText(text.c_str())) Notice("Build details copied to the clipboard.", NoticeKind::Success);
        }
        FlowNext(false, button.x);
        if (SecondaryButton("open_logs", "Open logs folder", button, true, Icon::Folder))
        {
            const fs::path log = platform::SessionLogPath();
            const fs::path folder = log.empty() ? base_ / "logs" : log.parent_path();
            std::error_code error;
            fs::create_directories(folder, error);
            OpenPath(folder);
        }
        FlowNext(false, button.x);
        const char* notes = identity.origin == BuildOrigin::Test ? "Pull request"
                          : identity.origin == BuildOrigin::Release ? "Release notes"
                                                                   : "All releases";
        if (SecondaryButton("build_notes", notes, button, true, Icon::GitHub)) SDL_OpenURL(BuildNotesUrl(identity).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", BuildNotesUrl(identity).c_str());
        EndCard();
    }

    Options options_;
    fs::path base_;
    std::string font_path_;
    bool capture_ = false;
    UiMetrics metrics_{};
    std::optional<ResolvedLaunch> frontend_launch_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* header_ = nullptr;
    SDL_Texture* aurora_logo_ = nullptr;
    float aurora_logo_width_ = 1, aurora_logo_height_ = 1;
    // Open standard gamepads (gamepad1-4 order); gamepad_ is the first.
    std::vector<SDL_Gamepad*> pads_;
    SDL_Gamepad* gamepad_ = nullptr;
    std::vector<std::string> gamepad_names_;
    // Triggers past half travel per pad, so a held trigger is not a new press;
    // the same for stick directions.
    std::map<SDL_JoystickID, std::array<bool, 2>> trigger_held_;
    std::map<SDL_JoystickID, std::array<bool, platform::GamepadStickDirectionCount>> stick_held_;
    std::set<std::pair<SDL_JoystickID, platform::GamepadInput>> pad_inputs_held_;
    bool pad_capture_release_ = false;
    // Wii Remote / DolphinBar detection for the Controls and Play pages (non-blocking).
    platform::WiimoteHidProbe wii_probe_;
    // Windows: Bluetooth Wii Remotes; scans continuously while Controls is open.
    platform::WiimoteBluetoothSearch bluetooth_search_;
    platform::WiimoteHidStatus wii_status_{};
    // The player each connected remote shows on its lights (-1: none).
    std::map<std::string, int> remote_players_;
    std::unique_ptr<platform::WiimotePlayerLights> remote_lights_;
    std::set<std::string> wii_held_; // remotes kept through one missed status answer
    // Controls: keyboard & mouse keys view, and the keycap waiting for a key.
    bool keyboard_view_ = false;
    int capture_action_ = -1, capture_slot_ = -1;
    int hotkey_capture_ = -1; // Hotkeys card: the HotkeyIndex waiting for a key
    // Display page: the Modern picture (clean or sharp) to return to from Original TV.
    std::string modern_picture_ = "sharp";
    // Gamepad profiles view: the profile being edited, its rename field and
    // the input waiting for a gamepad press.
    bool gamepad_view_ = false;
    int edit_profile_ = -1; // -1: the built-in Default
    std::size_t pad_slot_ = 0; // the gamepad the page was opened for
    bool renaming_ = false, rename_focus_ = false, confirm_delete_ = false;
    std::string rename_buffer_;
    int pad_capture_action_ = -1, pad_capture_slot_ = -1;
    // Gyro pointer preview on the profile page: the pad whose gyro it shows,
    // its readings and the preview pointer (0..1 of the box).
    SDL_Gamepad* gyro_preview_pad_ = nullptr;
    platform::GyroMotion gyro_preview_motion_;
    Uint64 gyro_preview_ns_ = 0;
    float gyro_preview_x_ = 0.5f, gyro_preview_y_ = 0.5f;
    bool gyro_preview_recenter_held_ = false, gyro_preview_drawn_ = false;
    bool wii_known_ = false;
    Uint64 wii_next_probe_ns_ = 0;
    float header_width_ = 1920;
    float header_height_ = 620;
    SDL_FRect hero_{};
    bool hero_visible_ = false;
    bool window_ui_ready_ = false;
    bool renderer_ui_ready_ = false;
    ConfigFile file_;
    Settings draft_;
    bool config_ok_ = false;
    bool dirty_ = false;
    bool play_prompt_ = false;
    NoticeKind notice_kind_ = NoticeKind::Info;
    std::string notice_;
    int page_ = PagePlay;
    bool close_requested_ = false;
    bool pending_ = false;
    bool region_prompt_ = false;
    DiscCheck check_;
    // The PC's languages, for Automatic (read once).
    std::vector<std::string> pc_languages_ = mscharged::platform::PcLanguages();
    std::future<DiscCheck> future_;
    // An image chosen with Browse, a drop or Locate, checked before it is pinned.
    std::future<DiscCheck> candidate_;
    bool candidate_pending_ = false;
    DiscInfo region_prompt_info_{};
    Uint64 missing_probe_ = 0, folders_probe_ = 0;
    // The image library (#65): the last finished scan and the running one.
    fs::path library_data_;
    ImageCache image_cache_;
    std::vector<LibraryImage> library_;
    std::vector<LibraryFolder> library_folders_, library_scan_folders_;
    std::vector<bool> library_available_;
    std::size_t library_other_games_ = 0;
    std::vector<fs::path> library_strikers_; // Super Mario Strikers images of the game folder
    bool library_limited_ = false;
    std::string scanned_folder_; // the game folder setting of the last scan
    bool scanned_subfolders_ = true;
    std::future<LibraryScan> library_scan_;
    std::shared_ptr<ScanProgress> library_progress_;
    bool library_scanning_ = false, library_rescan_ = false;
    // Who chooses an image when the next scan is done, if nothing is chosen then.
    enum class AutoChoice { None, FirstStart, NewFolder };
    AutoChoice autochoose_ = AutoChoice::None;
    LibrarySort library_sort_ = LibrarySort::Release;
    bool library_descending_ = false;
    std::map<std::string, SDL_Texture*> banners_; // by disc ID
    std::set<std::string> banner_failures_, banner_missing_;
    // The background search for a disc image when none is chosen.
    std::future<launcher::DiscSearchResult> search_;
    // The PC's graphics cards (Advanced -> Graphics card), listed once at start.
    std::future<std::vector<platform::GraphicsCard>> graphics_cards_scan_;
    std::vector<platform::GraphicsCard> graphics_cards_;
    bool graphics_cards_listing_ = false;
    bool searching_ = false;
    std::string search_from_;
    std::vector<std::string> displays_;
    std::vector<SDL_DisplayID> display_ids_;
    std::shared_ptr<DialogResult> dialog_ = std::make_shared<DialogResult>();
    std::shared_ptr<DialogResult> folder_dialog_ = std::make_shared<DialogResult>();
    std::shared_ptr<DialogResult> gamecube_dialog_ = std::make_shared<DialogResult>();
    FolderPurpose folder_purpose_ = FolderPurpose::SaveFolder;
    // The texture packs folder's packs (Gameplay -> Mods), read in the background.
    std::future<std::vector<platform::TexturePack>> texture_scan_;
    std::vector<platform::TexturePack> texture_packs_;
    std::string texture_scanned_folder_;
    bool texture_scanning_ = false, texture_rescan_ = false, texture_page_open_ = false;
    // A pack dropped onto the page, being copied into the folder: an error, or empty.
    std::future<std::string> pack_copy_;
    std::string pack_copy_name_;
    std::size_t pack_copy_textures_ = 0;
    fs::path import_source_;
    bool import_prompt_ = false;
    // Update check and installation (#50).
    std::unique_ptr<UpdateService> updates_;
    UpdateService::Status update_;
    bool update_dialog_ = false, update_capture_dialog_ = false, update_capture_install_ = false, restart_ = false;
    bool update_preview_ = false;
    UpdatePrompt update_prompt_ = UpdatePrompt::None;
    // The Test menu: open it next frame; whether it was open, closed with a
    // choice, and when it closed.
    bool test_menu_ = false, test_menu_open_ = false, test_menu_chose_ = false;
    bool show_hidden_tests_ = false; // Ctrl+click on Test: also hidden test builds, this run only
    double test_menu_closed_ = -1.0;
    // The update choice before a switch that is not settled yet (BeginSwitch).
    struct UpdateChoice
    {
        std::string channel;
        int test_build = 0;
    };
    std::optional<UpdateChoice> switch_from_;
    std::string notes_source_;
    std::vector<NoteBlock> notes_;
};
}

int main(int argc, char** argv)
{
    // The log window's own process (Advanced > Log window).
    if (mscharged::platform::IsLogWindowProcess(argc, argv)) return mscharged::platform::RunLogWindow(argc, argv);
#ifdef MSCHARGED_WINDOWS_GAME_MODULE_BASE
    // The game module's place below 2 GB, before the launcher's and the game's
    // graphics allocate into the low addresses (#98). 32 MiB is several times
    // the modules' image size.
    mscharged::platform::HoldNativeModuleBase(MSCHARGED_WINDOWS_GAME_MODULE_BASE, 32u << 20);
#endif
    mscharged::platform::ConnectStandardStreams();
    Options options;
    SetAppName();
#ifdef __APPLE__
    if (int code = 0; mscharged::platform::LeaveAppTranslocation(code)) return code;
#endif
    try
    {
    for (int i = 1; i < argc; ++i)
    {
        if (ParseLaunchOption(argc, argv, i, options.launch)) continue;
        const std::string arg = argv[i];
        if (arg == "--version")
        {
            std::cout << "Recharged " << mscharged::build::version << " (" << MSCHARGED_BUILD_CONFIG;
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
            std::cout << "; original frontend runtime)\n";
#else
            std::cout << "; launcher)\n";
#endif
            std::cout << DescribeBuild(CurrentBuild()) << '\n';
            return 0;
        }
        if (arg == "--help")
        {
            std::cout << "Usage: recharged [launch settings] [--launcher] [--version]\n" << LaunchOptionsHelp <<
                         "Supplying --disc/--disk starts the original game runtime directly when included in this build.\n"
                         "Use --launcher to open settings; launcher checks and explicit runtime modes keep their selected mode.\n"
                         "Development checks: --smoke-test | --screenshot FILE [--page play|game|display|audio|controls|advanced|about]\n";
#ifdef MSCHARGED_HAS_GAME_STARTUP
            std::cout << "Original startup prototype: --experimental-startup [--config FILE] (not playable)\n";
#endif
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
            std::cout << "Original Boot/Intro test: --experimental-frontend [--disk FILE] [--window] (startup and game sound incomplete)\n";
#endif
#ifdef MSCHARGED_HAS_ORIGINAL_SH_MENUS
            std::cout << "Original Options test: --experimental-options [--disk FILE] [--window] (source task diagnostic)\n";
#endif
#ifdef MSCHARGED_HAS_SCENE_PREVIEW
            std::cout << "Static Wii asset preview: --experimental-scene [--config FILE] [--frames N [--frame-timeout SECONDS]]\n"
                         "                        [--model /DISC/PATH.rlg] [--textures /DISC/PATH.rlt] [--model-id HEX]\n"
                         "                        [--world /DISC/gameworld.tmp.zlib --model-id HEX]\n"
                         "                        [--world /DISC/gameworld.tmp.zlib --world-res /DISC/gameworld.res.zlib\n"
                         "                         --object-id HEX ...] [--no-world-culling] (selected static objects)\n"
                         "                        [--frontend-world] (available static frontend objects; no menu)\n"
                         "                        [--frontend-layout /Art/fe/SCENE.fen] (stored text inspection)\n"
                         "                        [--frontend-frame /Art/fe/SCENE.fen [--frontend-slide NAME]] (static image/text layout)\n"
                         "                        [--frontend-images main|ingame|boot] (image bundle context; default main)\n"
                         "                        [--frontend-animate] (authored timeline; pause/reset in preview)\n"
                         "                        [--frontend-pointer Layer/Item] (rendered bounds/input inspection; no menu action)\n"
                         "                        [--frontend-boot] (retail boot screen; stops at unavailable services)\n"
                         "                        [--frontend-title] (original Title to Main; Intro/full startup pending)\n"
                         "                        [--frontend-main] (Main/Options/Audio/Visual/Credits; full startup pending)\n"
                         "                        [--frontend-options] (open Options directly)\n"
                         "                        [--character-shock] (original Bowser skin and FE animation)\n"
                         "                        [--particles] (authored emitter groups; pause/reset in preview)\n"
                         "                        [--camera /DISC/camera.cam | --debug-camera]\n"
                         "                        [--nis-primary /DISC/primary.nis --nis-secondary /DISC/secondary.nis]\n"
                         "                        [--pip-expand SECONDS] (camera-only PIP; no NIS actors)\n"
                         "                        [--unlit] [--shadow-textures /DISC/PATH.rlt --shadow-id HEX]\n";
#endif
            return 0;
        }
        if (arg == "--smoke-test") options.smoke_test = true;
        else if (arg == "--launcher") options.launcher = true;
        else if (arg == "--experimental-startup") options.experimental_startup = true;
        else if (arg == "--experimental-scene") options.experimental_scene = true;
        else if (arg == "--experimental-frontend") options.experimental_frontend = true;
        else if (arg == "--experimental-options") options.experimental_options = true;
        else if (arg == "--native-send" || arg == "--diagnostic-frame" || arg == "--resize-check") options.frame_arguments = true;
        else if (arg == "--frontend-world") { options.scene_arguments = true; options.scene.frontend_world = true; }
        else if (arg == "--frontend-animate") { options.scene_arguments = true; options.scene.frontend_animate = true; }
        else if (arg == "--frontend-boot") { options.scene_arguments = true; options.scene.frontend_boot = true; }
        else if (arg == "--frontend-title") { options.scene_arguments = true; options.scene.frontend_title = true; }
        else if (arg == "--frontend-main") { options.scene_arguments = true; options.scene.frontend_main = true; }
        else if (arg == "--frontend-options") { options.scene_arguments = true; options.scene.frontend_options = true; }
        else if (arg == "--character-shock") { options.scene_arguments = true; options.scene.character_shock = true; }
        else if (arg == "--particles") { options.scene_arguments = true; options.scene.particles = true; }
        else if (arg == "--no-world-culling") { options.scene_arguments = true; options.scene.no_world_culling = true; }
        else if (arg == "--debug-camera") { options.scene_arguments = true; options.scene.debug_camera = true; }
        else if (arg == "--unlit") { options.scene_arguments = true; options.scene.unlit = true; }
        else if ((arg == "--frames" || arg == "--frame-timeout" || arg == "--model" || arg == "--textures" || arg == "--model-id"
                  || arg == "--world" || arg == "--world-res" || arg == "--object-id"
                  || arg == "--nis-primary" || arg == "--nis-secondary" || arg == "--pip-expand"
                  || arg == "--frontend-frame" || arg == "--frontend-slide" || arg == "--frontend-images"
                  || arg == "--frontend-pointer"
                  || arg == "--camera" || arg == "--frontend-layout" || arg == "--shadow-textures" || arg == "--shadow-id") && i + 1 < argc)
        {
            options.scene_arguments = true;
            const std::string value = argv[++i];
            if (arg == "--model" || arg == "--textures") options.standalone_assets = true;
            if (arg == "--world") options.scene.world = value;
            else if (arg == "--world-res") options.scene.world_res = value;
            else if (arg == "--camera") options.scene.camera = value;
            else if (arg == "--nis-primary") options.scene.nis_primary = value;
            else if (arg == "--nis-secondary") options.scene.nis_secondary = value;
            else if (arg == "--pip-expand")
            {
                float seconds = 0;
                const auto parsed = FromCharsFloat(value.data(), value.data() + value.size(), seconds);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                    || !std::isfinite(seconds) || seconds <= 0 || seconds > 60)
                { std::cerr << "PIP expansion must be between 0 and 60 seconds (exclusive of zero).\n"; return 2; }
                options.scene.pip_expand = seconds;
            }
            else if (arg == "--frontend-layout") options.scene.frontend_layout = value;
            else if (arg == "--frontend-frame") options.scene.frontend_frame = value;
            else if (arg == "--frontend-slide") options.scene.frontend_slide = value;
            else if (arg == "--frontend-images") options.scene.frontend_images = value;
            else if (arg == "--frontend-pointer") options.scene.frontend_pointer = value;
            else if (arg == "--model") options.scene.model = value;
            else if (arg == "--textures") options.scene.textures = value;
            else if (arg == "--shadow-textures") options.scene.shadow_textures = value;
            else
            {
                std::uint32_t number = 0;
                std::string_view digits = value;
                const bool hex = arg == "--model-id" || arg == "--shadow-id" || arg == "--object-id";
                if (hex && (digits.size() >= 2 && digits.substr(0, 2) == "0x")) digits.remove_prefix(2);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number, hex ? 16 : 10);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()
                    || (arg == "--frames" && (!number || number > 10000))
                    || (arg == "--frame-timeout" && (!number || number > 600)))
                { std::cerr << "Invalid " << arg << " value: " << value << '\n'; return 2; }
                if (arg == "--frames") options.scene.frames = number;
                else if (arg == "--frame-timeout") options.scene.frame_timeout = number;
                else if (arg == "--object-id")
                {
                    if (options.scene.object_ids.size() == 256)
                    { std::cerr << "World preview accepts at most 256 object IDs.\n"; return 2; }
                    options.scene.object_ids.push_back(number);
                }
                else if (arg == "--shadow-id") options.scene.shadow_id = number;
                else options.scene.model_id = number;
            }
        }
        else if ((arg == "--screenshot" || arg == "--page") && i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (arg == "--screenshot") options.screenshot = PathFromUtf8(value);
            else
            {
                const auto it = std::find(std::begin(page_ids), std::end(page_ids), value);
                if (it == std::end(page_ids)) { std::cerr << "Unknown launcher page: " << value << '\n'; return 2; }
                options.page = int(it - std::begin(page_ids));
                options.page_selected = true;
            }
        }
        else { std::cerr << "Unknown or incomplete argument: " << arg << ". Use --help.\n"; return 2; }
    }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    // logs/recharged.log for player runs (not the development smoke/screenshot checks).
    if (!options.smoke_test && options.screenshot.empty())
    {
        mscharged::platform::StartSessionLog(mscharged::build::version, argc, argv);
        mscharged::platform::SessionLogLine("Build: " + DescribeBuild(CurrentBuild()));
    }
    options.config = options.launch.config;
    unsigned runtime_modes = unsigned(options.experimental_startup)
        + unsigned(options.experimental_scene) + unsigned(options.experimental_frontend)
        + unsigned(options.experimental_options);
    const bool launcher_requested = options.launcher || options.smoke_test
        || !options.screenshot.empty() || options.page_selected;
    if (!runtime_modes && options.launch.disc && !launcher_requested)
    {
        options.experimental_frontend = true;
        runtime_modes = 1;
    }
    if (runtime_modes && (runtime_modes > 1 || launcher_requested))
    { std::cerr << "Select one runtime mode; --launcher and capture/smoke/page options select the launcher.\n"; return 2; }
    if (options.frame_arguments && !options.experimental_frontend && !options.experimental_options)
    { std::cerr << "Frame/resize options require --experimental-frontend or --experimental-options.\n"; return 2; }
    if (options.scene_arguments && !options.experimental_scene)
    { std::cerr << "Asset/frame options require --experimental-scene.\n"; return 2; }
    if (options.scene.frame_timeout && !options.scene.frames)
    { std::cerr << "--frame-timeout requires --frames.\n"; return 2; }
    if (options.scene.frontend_pointer && (!options.scene.frontend_frame || options.scene.frontend_pointer->empty()))
    { std::cerr << "--frontend-pointer requires --frontend-frame and a nonempty instance path.\n"; return 2; }
    if ((options.scene.frontend_title || options.scene.frontend_main || options.scene.frontend_options) && (options.standalone_assets || options.scene.model_id || options.scene.world
        || options.scene.world_res || !options.scene.object_ids.empty() || options.scene.frontend_world
        || options.scene.frontend_layout || options.scene.frontend_frame || options.scene.frontend_slide
        || options.scene.frontend_images || options.scene.frontend_pointer || options.scene.frontend_animate || options.scene.nis_primary
        || options.scene.nis_secondary || options.scene.pip_expand || options.scene.camera || options.scene.debug_camera
        || options.scene.shadow_id || options.scene.shadow_textures || options.scene.particles || options.scene.unlit
        || options.scene.no_world_culling || options.scene.character_shock || options.scene.frontend_boot
        || (unsigned(options.scene.frontend_title)+unsigned(options.scene.frontend_main)+unsigned(options.scene.frontend_options)>1)))
    { std::cerr << "Select one of --frontend-title, --frontend-main or --frontend-options with its own resources; use --frames, --frame-timeout or --config.\n"; return 2; }
    if (options.scene.frontend_boot && (options.standalone_assets || options.scene.model_id || options.scene.world
        || options.scene.world_res || !options.scene.object_ids.empty() || options.scene.frontend_world
        || options.scene.frontend_layout || options.scene.frontend_frame || options.scene.frontend_slide
        || options.scene.frontend_images || options.scene.frontend_pointer || options.scene.frontend_animate || options.scene.nis_primary
        || options.scene.nis_secondary || options.scene.pip_expand || options.scene.camera || options.scene.debug_camera
        || options.scene.shadow_id || options.scene.shadow_textures || options.scene.particles || options.scene.unlit
        || options.scene.no_world_culling || options.scene.character_shock))
    { std::cerr << "--frontend-boot selects its own retail scene and resources; use --frames, --frame-timeout or --config.\n"; return 2; }
    if (options.scene.character_shock && (options.standalone_assets || options.scene.model_id || options.scene.world
        || options.scene.world_res || !options.scene.object_ids.empty() || options.scene.frontend_world
        || options.scene.frontend_layout || options.scene.frontend_frame || options.scene.frontend_slide
        || options.scene.frontend_images || options.scene.frontend_pointer || options.scene.frontend_animate || options.scene.nis_primary
        || options.scene.nis_secondary || options.scene.pip_expand || options.scene.camera || options.scene.debug_camera
        || options.scene.shadow_id || options.scene.shadow_textures || options.scene.particles || options.scene.no_world_culling))
    { std::cerr << "--character-shock selects its own model, animation and camera; use --frames, --frame-timeout, --config or --unlit.\n"; return 2; }
    if (options.scene.nis_primary.has_value() != options.scene.nis_secondary.has_value()
        || (options.scene.pip_expand && !options.scene.nis_primary))
    { std::cerr << "PIP requires both --nis-primary and --nis-secondary.\n"; return 2; }
    if (options.scene.nis_primary && (options.scene.camera || options.scene.debug_camera || options.scene.shadow_id || options.scene.frontend_layout || options.scene.frontend_frame))
    { std::cerr << "PIP cannot be combined with other cameras, shadows or text inspection.\n"; return 2; }
    if (options.scene.frontend_layout && options.scene.debug_camera)
    { std::cerr << "--frontend-layout cannot be combined with --debug-camera; they use separate controls.\n"; return 2; }
    if ((options.scene.frontend_frame && options.scene.frontend_layout) || (options.scene.frontend_slide && !options.scene.frontend_frame))
    { std::cerr << "Select --frontend-frame or --frontend-layout; --frontend-slide requires --frontend-frame.\n"; return 2; }
    if (options.scene.frontend_images && (!options.scene.frontend_frame
        || (*options.scene.frontend_images != "main" && *options.scene.frontend_images != "ingame" && *options.scene.frontend_images != "boot")))
    { std::cerr << "--frontend-images requires --frontend-frame and main, ingame or boot.\n"; return 2; }
    if (options.scene.frontend_animate && !options.scene.frontend_frame)
    { std::cerr << "--frontend-animate requires --frontend-frame.\n"; return 2; }
    if (options.scene.particles && (options.scene.nis_primary || options.scene.shadow_id || options.scene.shadow_textures))
    { std::cerr << "--particles cannot be combined with PIP or shadow options.\n"; return 2; }
    if (options.scene.debug_camera && (options.scene.camera || options.scene.shadow_id || options.scene.shadow_textures))
    { std::cerr << "--debug-camera cannot be combined with --camera or shadow options.\n"; return 2; }
    if (options.scene.shadow_id.has_value() != options.scene.shadow_textures.has_value())
    { std::cerr << "--shadow-textures and --shadow-id must be supplied together.\n"; return 2; }
    if (options.scene.frontend_world && (options.scene.world || options.scene.world_res || !options.scene.object_ids.empty()
        || options.scene.model_id || options.scene.shadow_id || options.standalone_assets))
    { std::cerr << "--frontend-world cannot be combined with explicit model, world or shadow selections.\n"; return 2; }
    if (options.scene.world_res || !options.scene.object_ids.empty())
    {
        if (!options.scene.world || !options.scene.world_res || options.scene.object_ids.empty())
        { std::cerr << "World objects require --world, --world-res and at least one --object-id.\n"; return 2; }
        if (options.scene.model_id || options.scene.shadow_id || options.standalone_assets)
        { std::cerr << "World object selection cannot be combined with model or shadow lookup options.\n"; return 2; }
    }
    if (options.scene.no_world_culling && !options.scene.world_res && !options.scene.frontend_world)
    { std::cerr << "--no-world-culling requires a world object selection.\n"; return 2; }
    if (options.scene.world && !options.scene.model_id && !options.scene.world_res)
    { std::cerr << "--world requires an explicit --model-id.\n"; return 2; }
    if (options.scene.world && options.standalone_assets)
    { std::cerr << "Select --world or separate --model/--textures assets.\n"; return 2; }
    try
    {
        if (options.experimental_frontend || options.experimental_options)
        {
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
#ifndef MSCHARGED_HAS_ORIGINAL_SH_MENUS
            if(options.experimental_options) {
                std::cerr << "Original Options requires MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS=ON.\n";
                return 2;
            }
#endif
            std::vector<char*> arguments;
            for (int i = 0; i < argc; ++i)
                if (std::string_view(argv[i]) != "--experimental-frontend" &&
                    std::string_view(argv[i]) != "--experimental-options") arguments.push_back(argv[i]);
            arguments.push_back(nullptr);
            return RunOriginalGame(static_cast<int>(arguments.size()) - 1, arguments.data(),
                nullptr, false, options.experimental_options ?
                    OriginalMainScene::FrontendOptions : OriginalMainScene::FrontendSequence, GameTitle().c_str());
#else
            std::cerr << "Original game runtime is not included in this build. "
                         "A game-enabled build is required for direct disc startup; --launcher opens settings.\n";
            return 2;
#endif
        }
        if (options.experimental_scene)
        {
#ifdef MSCHARGED_HAS_SCENE_PREVIEW
            if (options.config.empty())
            {
                const char* base = SDL_GetBasePath();
                Require(base != nullptr, "Cannot locate the executable");
                options.config = DefaultConfig(PathFromUtf8(base));
            }
            options.launch.config = options.config;
            const auto launch = LoadLaunch(options.launch, options.config.parent_path());
            return RunScenePreview(argc, argv, options.config, options.scene, nullptr, &launch);
#else
            std::cerr << "Static asset preview is not in this build. Use cmake --workflow --preset scene.\n";
            return 2;
#endif
        }
        if (options.experimental_startup)
        {
#ifdef MSCHARGED_HAS_GAME_STARTUP
            if (options.config.empty())
            {
                const char* base = SDL_GetBasePath();
                Require(base != nullptr, "Cannot locate the executable");
                options.config = DefaultConfig(PathFromUtf8(base));
            }
            options.launch.config = options.config;
            return RunGameStartup(argc, argv, LoadLaunch(options.launch, options.config.parent_path()));
#else
            std::cerr << "Experimental startup is not in this build. Use cmake --workflow --preset startup.\n";
            return 2;
#endif
        }
        std::optional<ResolvedLaunch> frontend_launch;
        int result;
        fs::path restart, reopen;
        {
            Launcher launcher;
            result = launcher.Run(std::move(options));
            frontend_launch = launcher.FrontendLaunch();
            if (launcher.RestartRequested()) restart = launcher.RestartExecutable();
            else if (frontend_launch && frontend_launch->settings.reopen_after_game) reopen = launcher.RestartExecutable();
        } // Destroy the launcher/ImGui/SDL session before Aurora initializes.
        if (!restart.empty())
        {
            // An update replaced the program files: start the new launcher.
            mscharged::platform::EndSessionLog("restarting after an update");
            const std::string error = RestartLauncher(restart, argc, argv);
            if (error.empty()) return 0;
            mscharged::platform::ReportFatalError("The update is installed, but the launcher could not start again ("
                + error + "). Please start it again yourself.");
            return 1;
        }
        SetAppName();
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
        // [launcher] reopen_after_game: closing the game starts this launcher again.
        if (!reopen.empty()) mscharged::platform::ArmLauncherReopen(reopen, argc, argv);
        if (frontend_launch) return RunOriginalGame(argc, argv, &*frontend_launch, true,
            OriginalMainScene::FrontendSequence, GameTitle().c_str());
#endif
        return result;
    }
    catch (const std::exception& e)
    {
        mscharged::platform::ReportFatalError(std::string("Launcher failed: ") + e.what());
        return 1;
    }
}
