#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_stdlib.h"

#include "bootstrap/launch_options.h"
#include "launcher/ui_kit.h"
#include "launcher/ui_metrics.h"
#include "mscharged/build_version.h"
#include "platform/parse_float.h"
#include "platform/window_fit.h"
#include "platform/disc.h"
#include "platform/app_icon.h"
#include "platform/path.h"
#include "platform/session_log.h"
#include "platform/gamepad_bindings.h"
#include "platform/key_bindings.h"
#include "platform/wiimote_hid.h"
#ifdef MSCHARGED_HAS_GAME_STARTUP
#include "runtime/startup.h"
#endif
#include "runtime/scene.h"
#ifdef MSCHARGED_HAS_ORIGINAL_CREDITS
#include "runtime/original_main_credits.h"
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
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mscharged;
using namespace mscharged::launcher;

namespace
{
// Window title of the game and the launcher: the port's name and build.
std::string GameTitle() { return std::string("MSCharged Port ") + mscharged::build::version; }

enum Page { PagePlay, PageGame, PageDisplay, PageAudio, PageControls, PageAdvanced, PageAbout, PageCount };
constexpr const char* page_names[] = {"Play", "Game", "Display", "Audio", "Controls", "Advanced", "About"};
constexpr const char* page_ids[] = {"play", "game", "display", "audio", "controls", "advanced", "about"};
constexpr Icon page_icons[] = {Icon::Play, Icon::Disc, Icon::Display, Icon::Audio, Icon::Keyboard, Icon::Sliders,
                               Icon::Info};
static_assert(std::size(page_names) == PageCount && std::size(page_ids) == PageCount
    && std::size(page_icons) == PageCount);

struct Options
{
    fs::path config;
    LaunchOptions launch;
    fs::path screenshot;
    bool launcher = false;
    bool smoke_test = false;
    bool experimental_startup = false;
    bool experimental_scene = false;
    bool experimental_credits = false;
    bool experimental_frontend = false;
    bool experimental_options = false;
    bool credits_arguments = false;
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

struct DiscCheck
{
    std::optional<DiscInfo> info;
    std::string error;
    std::string selection;
};

enum class NoticeKind { Info, Success, Warning, Error };

// Region branches follow original Game/main.cpp; the native runtime currently
// supports the USA release only.
const char* RegionName(const std::string& id) { return DiscRegionName(id); }

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
    const std::optional<ResolvedLaunch>& StartupLaunch() const { return startup_launch_; }
    const std::optional<ResolvedLaunch>& CreditsLaunch() const { return credits_launch_; }
    const std::optional<ResolvedLaunch>& FrontendLaunch() const { return frontend_launch_; }

    ResolvedLaunch EffectiveLaunch() const { return ResolveLaunch(file_, options_.launch, &draft_); }
    std::string DiscSelection() const { return PathUtf8(EffectiveLaunch().disc_path); }

    ~Launcher()
    {
        wii_probe_.Close();
        live_.Close();
        if (renderer_ui_ready_) ImGui_ImplSDLRenderer3_Shutdown();
        if (window_ui_ready_) ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        for (auto* pad : pads_) SDL_CloseGamepad(pad);
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
        page_ = options_.page;
        capture_ = options_.smoke_test || !options_.screenshot.empty();

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
        if (config_ok_ && !EffectiveLaunch().disc_path.empty() && !options_.smoke_test) CheckDisc();

        bool quit = false;
        unsigned frame = 0;
        const unsigned smoke_frames = unsigned(PageCount) * 4 + 2;
        while (!quit)
        {
            const Uint64 frame_start = SDL_GetTicks();
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                    && event.window.windowID == SDL_GetWindowID(window_)))
                    close_requested_ = true;
                if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
                    RefreshGamepad();
                if (event.type == SDL_EVENT_DISPLAY_ADDED || event.type == SDL_EVENT_DISPLAY_REMOVED)
                    RefreshDisplays();
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                    SelectDisc(event.drop.data);
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && capture_action_ >= 0)
                    CaptureKey(event.key.scancode);
                GamepadCaptureEvent(event);
            }
            // While an input waits for a gamepad press, the gamepad does not
            // drive the launcher.
            auto& io = ImGui::GetIO();
            if (pad_capture_action_ >= 0) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
            else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            PollResults();
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
            if (!options_.screenshot.empty() && frame >= 12 && (!pending_ || frame > 600))
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
            dirty_ = false;
            config_ok_ = true;
            check_ = {};
            Notice(file_.exists ? "Settings loaded." : "Welcome! Choose your game disc to get started.", NoticeKind::Info);
        }
        catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); config_ok_ = false; }
    }

    bool Save()
    {
        try
        {
            SaveConfig(file_, draft_);
            dirty_ = false;
            Notice("Settings saved.", NoticeKind::Success);
            return true;
        }
        catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); return false; }
    }

    void SelectDisc(const std::string& path)
    {
        if (options_.launch.disc)
        {
            Notice("The disc from the command line is used for this run.", NoticeKind::Info);
            return;
        }
        std::error_code error;
        const auto absolute = fs::absolute(PathFromUtf8(path));
        auto relative = fs::relative(absolute, file_.path.parent_path(), error);
        draft_.disc = PathUtf8(error ? absolute : relative);
        dirty_ = true;
        check_ = {};
        if (!pending_) CheckDisc();
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
            future_ = std::async(std::launch::async, [path, selection] {
                DiscCheck result;
                result.selection = selection;
                try { result.info = InspectDisc(path); }
                catch (const std::exception& e) { result.error = e.what(); }
                return result;
            });
        }
        catch (const std::exception& e) { pending_ = false; check_.error = e.what(); }
    }

    void Browse()
    {
        // "All files" too: a system that does not know the .rvz type (macOS) may
        // grey such files out under the first filter. The check reads the content.
        static const SDL_DialogFileFilter filters[] = {{"Wii disc images (ISO / RVZ)", "iso;rvz;ISO;RVZ"},
                                                       {"All files", "*"}};
        {
            std::lock_guard<std::mutex> lock(dialog_->mutex);
            dialog_->active = true;
        }
        SDL_ShowOpenFileDialog(DiscChosen, new std::shared_ptr<DialogResult>(dialog_), window_, filters,
                               int(std::size(filters)), nullptr, false);
    }

    void PollResults()
    {
        {
            std::lock_guard<std::mutex> lock(dialog_->mutex);
            if (dialog_->ready)
            {
                dialog_->ready = false;
                if (!dialog_->error.empty()) Notice(dialog_->error, NoticeKind::Error);
                else if (!dialog_->path.empty()) SelectDisc(dialog_->path);
            }
        }
        if (pending_ && future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto result = future_.get();
            pending_ = false;
            if (result.selection == DiscSelection())
            {
                check_ = std::move(result);
                region_prompt_ = check_.info && !SupportedRelease(*check_.info);
            }
        }
    }

    // Standard gamepads in the order the game numbers them (gamepad1-4);
    // Wii Remotes have their own driver and are left out.
    void RefreshGamepad()
    {
        // Every pad stays open: the gamepad setup takes presses from any of them.
        for (auto* pad : pads_) SDL_CloseGamepad(pad);
        pads_.clear();
        gamepad_ = nullptr;
        gamepad_names_.clear();
        trigger_held_.clear();
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
        if (!config_ok_) return "Fix the settings file problem shown below first.";
        if (pending_) return "Checking your disc...";
        if (DiscSelection().empty()) return "Choose your Mario Strikers Charged disc image (ISO or RVZ).";
        if (!check_.info || check_.selection != DiscSelection())
            return check_.error.empty() ? "Check your disc on the Game page." : check_.error;
        if (!SupportedRelease(*check_.info))
            return "Only the USA version of Mario Strikers Charged (R4QE01) is supported for now.";
        const auto settings = EffectiveLaunch().settings;
        if (settings.aspect != "auto" && settings.aspect != "4:3" && settings.aspect != "16:9")
            return "Choose Automatic, 4:3 or 16:9 as the aspect ratio on the Display page.";
        if (settings.language != "auto" && settings.language != "english" && settings.language != "french"
            && settings.language != "spanish")
            return "Choose a language this release supports on the Game page.";
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
#ifdef _WIN32
        const auto executable = base_ / "mscharged.exe";
#else
        const auto executable = base_ / "mscharged";
#endif
        std::string command = Quote(PathUtf8(executable));
        if (!launch.disc_path.empty()) command += " --disc " + Quote(PathUtf8(launch.disc_path));
        command += s.fullscreen ? " --fullscreen" : " --window";
        command += " --size " + std::to_string(s.width) + "x" + std::to_string(s.height);
        command += " --aspect " + s.aspect;
        return command;
    }

    // ------------------------------------------------------------- layout
    void Draw(bool& quit)
    {
        if (calibration_.active)
        {
            DrawCalibration();
            return;
        }
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
        DrawRegionPrompt();
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
        DrawTracked(draw, fonts.caption, {pad, y}, Col(color::muted), "MARIO STRIKERS", Dp(2.4f));
        y += FontSize(fonts.caption) + Dp(2);
        // "CHARGED" with a soft glow and a vertical lime-to-green gradient.
        for (int i = 3; i >= 1; --i)
            DrawLabel(draw, fonts.title, {pad, y + Dp(float(i) * 0.7f)}, Col(color::accent, 0.08f * float(i)), "CHARGED");
        const int start = draw->VtxBuffer.Size;
        DrawLabel(draw, fonts.title, {pad, y}, IM_COL32_WHITE, "CHARGED");
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, {pad, y},
                                                     {pad, y + FontSize(fonts.title)}, Col(color::accent_hi), Col(color::accent));
        y += FontSize(fonts.title) + Dp(4);
        DrawLabel(draw, fonts.caption, {pad, y}, Col(color::dim), "Native PC port");
        y += FontSize(fonts.caption) + Dp(28);

        ImGui::SetCursorPos({Dp(12), y});
        for (int i = 0; i < PageCount; ++i)
        {
            ImGui::SetCursorPosX(Dp(12));
            if (NavItem(page_names[i], page_icons[i], page_ == i, width - Dp(24))) page_ = i;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + Dp(4));
        }

        // Version at the bottom of the sidebar.
        const float badge = height - Dp(40);
        if (ImGui::GetCursorPosY() < badge)
        {
            ImGui::SetCursorPos({pad, badge});
            ImGui::PushFont(fonts.caption);
            ImGui::PushStyleColor(ImGuiCol_Text, color::dim);
            ImGui::TextUnformatted(build::version);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }

    void DrawMain(ImVec2 origin, ImVec2 size, bool& quit)
    {
        if (page_ != PageControls && keyboard_view_) CloseKeyboardView();
        if (page_ != PageControls && gamepad_view_) CloseGamepadView();
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
            case PageAdvanced: AdvancedPage(quit); break;
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
        y += FontSize(fonts.caption) + Dp(10);
        DrawTracked(draw, fonts.overline, {origin.x + pad, y}, IM_COL32_WHITE, "MARIO STRIKERS", Dp(3.0f));
        y += FontSize(fonts.overline) - Dp(4);
        for (int i = 4; i >= 1; --i)
            DrawLabel(draw, fonts.hero, {origin.x + pad, y + Dp(float(i))}, Col(color::accent, 0.07f * float(i)), "CHARGED");
        const int start = draw->VtxBuffer.Size;
        DrawLabel(draw, fonts.hero, {origin.x + pad, y}, IM_COL32_WHITE, "CHARGED");
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, {origin.x, y},
                                                     {origin.x, y + FontSize(fonts.hero)}, Col(color::accent_hi),
                                                     Col(color::accent));
        y += FontSize(fonts.hero) + Dp(2);
        DrawLabel(draw, fonts.label, {origin.x + pad, y}, Col(color::text, 0.82f),
                  "The original game, rebuilt to run natively on your PC.");

        // Play button and status, overlapping the bottom of the artwork.
        const std::string blocker = PlayBlocker();
        const float button_y = hero_max.y - Dp(30);
        ImGui::SetCursorScreenPos({origin.x + pad, button_y});
        if (PrimaryButton("play", "PLAY", Dp(230, 62), blocker.empty(), Icon::Play, true)) Play(quit);
        if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", blocker.c_str());
        const float status_x = origin.x + pad + Dp(230) + Dp(24);
        ImGui::SetCursorScreenPos({status_x, button_y + Dp(8)});
        DrawPlayStatus(blocker);

        // Summary cards.
        const float cards_y = button_y + Dp(62) + Dp(30);
        const float footer = Dp(30);
        if (cards_y + Dp(118) + footer + Dp(12) <= origin.y + size.y)
        {
            const float gap = Dp(16);
            const float width = (size.x - pad * 2 - gap * 2) / 3.0f;
            ImGui::SetCursorScreenPos({origin.x + pad, cards_y});
            SummaryCard("##disc_card", "Game disc", Icon::Disc, width, PageGame, DiscSummary());
            ImGui::SameLine(0, gap);
            SummaryCard("##display_card", "Display", Icon::Display, width, PageDisplay, DisplaySummary());
            ImGui::SameLine(0, gap);
            SummaryCard("##language_card", "Language & audio", Icon::Audio, width, PageAudio, LanguageSummary());
        }
    }

    void DrawPlayStatus(const std::string& blocker)
    {
        const auto& fonts = CurrentFonts();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        ImVec4 tint = color::accent;
        Icon icon = Icon::Check;
        std::string headline = "Ready to play";
        std::string detail;
        if (pending_)
        {
            tint = color::info;
            icon = Icon::None;
            headline = "Checking your disc";
            detail = "Reading the game partition...";
            Spinner({pos.x + Dp(11), pos.y + Dp(12)}, Dp(8), Col(tint));
        }
        else if (!blocker.empty())
        {
            tint = DiscSelection().empty() ? color::info : color::warning;
            icon = DiscSelection().empty() ? Icon::Disc : Icon::Warning;
            headline = DiscSelection().empty() ? "Choose your game disc" : "Not ready yet";
            detail = blocker;
        }
        else if (check_.info)
            detail = "Mario Strikers Charged  -  " + std::string(RegionName(check_.info->game_id)) + "  -  "
                + check_.info->game_id + " rev " + std::to_string(unsigned(check_.info->revision));
        if (icon != Icon::None) DrawIcon(draw, icon, {pos.x + Dp(11), pos.y + Dp(12)}, Dp(20), Col(tint));
        DrawLabel(draw, fonts.label, {pos.x + Dp(30), pos.y + Dp(1)}, Col(color::text), headline.c_str());
        if (!detail.empty())
        {
            const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - Dp(28);
            draw->AddText(fonts.caption, FontSize(fonts.caption), {pos.x + Dp(30), pos.y + Dp(26)}, Col(color::muted),
                          detail.c_str(), nullptr, std::max(Dp(120), limit - pos.x - Dp(30)));
        }
    }

    struct Summary { std::string value; std::string detail; ImVec4 tint; };

    Summary DiscSummary() const
    {
        if (pending_) return {"Checking...", "Reading the disc header", color::info};
        if (DiscSelection().empty()) return {"No disc selected", "Choose an ISO or RVZ image", color::warning};
        if (check_.info && check_.selection == DiscSelection())
        {
            const auto& info = *check_.info;
            return {info.game_id + "  -  " + RegionName(info.game_id),
                    "Revision " + std::to_string(unsigned(info.revision)) + "  -  " + info.format + "  -  "
                        + std::to_string(info.file_count) + " files",
                    SupportedRelease(info) ? color::accent : color::warning};
        }
        if (!check_.error.empty()) return {"Disc problem", check_.error, color::danger};
        return {PathUtf8(PathFromUtf8(DiscSelection()).filename()), "Not checked yet", color::muted};
    }

    Summary DisplaySummary() const
    {
        const auto s = EffectiveLaunch().settings;
        std::string aspect = s.aspect == "auto" ? "Automatic aspect" : s.aspect;
        return {s.fullscreen ? "Fullscreen" : std::to_string(s.width) + " x " + std::to_string(s.height) + " window",
                aspect + (s.vsync ? "  -  VSync on" : "  -  VSync off"), color::info};
    }

    Summary LanguageSummary() const
    {
        const auto s = EffectiveLaunch().settings;
        const std::string language = s.language == "auto" ? "Automatic" :
            std::string(1, char(std::toupper(static_cast<unsigned char>(s.language[0])))) + s.language.substr(1);
        return {language + " text", s.mute ? "Audio muted" : "Volume " + std::to_string(s.master_volume) + "%",
                color::info};
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

    // Why a readable Charged disc cannot play yet.
    std::string UnsupportedDiscReason(const DiscInfo& info) const
    {
        return std::string("Your disc is the ") + RegionName(info.game_id) + " version (" + info.game_id + ").";
    }

    // Shown once after a check finds a readable but unsupported disc.
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
            TextColored(fonts.heading, color::text, "Only the USA version is supported for now");
            ImGui::Dummy({0, Dp(4)});
            if (check_.info) TextWrappedColored(fonts.body, color::muted, UnsupportedDiscReason(*check_.info).c_str());
            ImGui::Dummy({0, Dp(4)});
            TextWrappedColored(fonts.body, color::text,
                "The port currently runs Mario Strikers Charged for the USA (R4QE01). The European, "
                "Japanese and Korean versions need their own decompilation work and should follow soon.");
            ImGui::Dummy({0, Dp(10)});
            if (PrimaryButton("region_ok", "OK", Dp(120, 42), true) || ImGui::IsKeyPressed(ImGuiKey_Escape)
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
            if (SecondaryButton("cancel_play", "Cancel", Dp(100, 42)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
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
            if (SecondaryButton("cancel", "Cancel", Dp(100, 42)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // -------------------------------------------------------------- pages
    void GamePage()
    {
        PageHeader("Game", "Your Mario Strikers Charged disc image. ISO and RVZ files are read directly; nothing is extracted.");
        if (options_.launch.disc || options_.launch.size || options_.launch.fullscreen || options_.launch.aspect)
        {
            Chip("Command-line options apply to this run only", color::info, Icon::Info);
            ImGui::Dummy({0, Dp(2)});
        }

        BeginCard("##disc", "Disc image", "Drop an image onto this window, or browse for it.", Icon::Disc);
        bool browsing;
        { std::lock_guard<std::mutex> lock(dialog_->mutex); browsing = dialog_->active; }
        const float buttons = Dp(118) + Dp(10) + Dp(118);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - Dp(10));
        if (options_.launch.disc)
        {
            auto selected = DiscSelection();
            ImGui::BeginDisabled();
            ImGui::InputText("##disc_path", &selected);
            ImGui::EndDisabled();
        }
        else if (ImGui::InputTextWithHint("##disc_path", "Path to R4QE01 .iso or .rvz", &draft_.disc))
        { dirty_ = true; check_ = {}; }
        ImGui::SameLine(0, Dp(10));
        if (SecondaryButton("browse", "Browse...", {Dp(118), ImGui::GetFrameHeight()},
                            !browsing && config_ok_ && !options_.launch.disc, Icon::Folder))
            Browse();
        ImGui::SameLine(0, Dp(10));
        if (SecondaryButton("check", pending_ ? "Checking" : "Check", {Dp(118), ImGui::GetFrameHeight()},
                            !pending_ && config_ok_ && !DiscSelection().empty(), Icon::Refresh))
            CheckDisc();
        ImGui::Dummy({0, Dp(4)});
        DiscStatusPanel();
        EndCard();

        BeginCard("##language", "Game language", "Menus and on-screen text follow the Wii system language.", Icon::Info);
        if (BeginRows("##language_rows"))
        {
            Row("Text language", "Automatic currently selects English. Saved with your settings.");
            LanguageControl();
            EndRows();
        }
        EndCard();

        BeginCard("##data", "Save data", "The game keeps its save file in a private folder next to the launcher.", Icon::Folder);
        const fs::path data = base_ / "original-main-credits-data";
        if (BeginRows("##data_rows"))
        {
            const std::string path = PathUtf8(data);
            Row("Save folder", path.c_str());
            if (SecondaryButton("open_data", "Open folder", {ImGui::GetContentRegionAvail().x, Dp(40)},
                                fs::exists(data), Icon::Folder))
                OpenPath(data);
            if (!fs::exists(data) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Created when you first play.");
            EndRows();
        }
        EndCard();
    }

    void DiscStatusPanel()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = Dp(86);
        draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, Col(Mix(color::field, color::card, 0.5f)), Dp(12));
        const ImVec2 icon_center{pos.x + Dp(40), pos.y + height * 0.5f};
        const float text_x = pos.x + Dp(76);
        auto line = [&](ImFont* font, float y, const ImVec4& tint, const std::string& text) {
            draw->AddText(font, FontSize(font), {text_x, pos.y + y}, Col(tint), text.c_str(), nullptr,
                          width - (text_x - pos.x) - Dp(16));
        };
        if (pending_)
        {
            Spinner(icon_center, Dp(14), Col(color::info));
            line(fonts.label, Dp(20), color::text, "Checking the disc");
            line(fonts.caption, Dp(46), color::muted, "Reading the disc header and the game file table...");
        }
        else if (check_.info && check_.selection == DiscSelection())
        {
            const auto& info = *check_.info;
            const bool supported = SupportedRelease(info);
            draw->AddCircleFilled(icon_center, Dp(22), Col(supported ? color::accent : color::warning, 0.15f));
            DrawIcon(draw, supported ? Icon::Check : Icon::Warning, icon_center, Dp(22),
                     Col(supported ? color::accent : color::warning));
            line(fonts.label, Dp(14), color::text, "Mario Strikers Charged  -  " + std::string(RegionName(info.game_id)));
            line(fonts.caption, Dp(40), color::muted, info.game_id + "  -  Revision " + std::to_string(unsigned(info.revision))
                + "  -  " + info.format + "  -  " + std::to_string(info.file_count) + " files");
            line(fonts.caption, Dp(60), supported ? color::accent : color::warning,
                 supported ? "Supported release. Ready to play."
                           : "Not supported yet: only the USA release (R4QE01) runs for now.");
        }
        else if (!check_.error.empty())
        {
            draw->AddCircleFilled(icon_center, Dp(22), Col(color::danger, 0.15f));
            DrawIcon(draw, Icon::Error, icon_center, Dp(22), Col(color::danger));
            line(fonts.label, Dp(20), color::text, "The disc could not be read");
            line(fonts.caption, Dp(46), color::muted, check_.error);
        }
        else
        {
            DrawIcon(draw, Icon::Disc, icon_center, Dp(26), Col(color::dim));
            line(fonts.label, Dp(20), color::text, DiscSelection().empty() ? "No disc selected" : "Not checked yet");
            line(fonts.caption, Dp(46), color::muted, "Choose your image and check that its game partition opens.");
        }
        ImGui::Dummy({width, height});
    }

    void LanguageControl()
    {
        const std::string id = check_.info ? check_.info->game_id : "R4QE01";
        const float width = ImGui::GetContentRegionAvail().x;
        if (id == "R4QE01")
        {
            const char* values[] = {"auto", "english", "french", "spanish"};
            int selected = -1;
            for (int i = 0; i < 4; ++i) if (draft_.language == values[i]) selected = i;
            if (Segmented("##language_choice", &selected, {"Auto", "English", "French", "Spanish"}, width))
            { draft_.language = values[selected]; dirty_ = true; }
            if (selected < 0)
                TextWrappedColored(CurrentFonts().caption, color::warning,
                                   "The saved language is not available in this release. Choose one above.");
            return;
        }
        struct Entry { const char* value; const char* name; };
        std::vector<Entry> entries{{"auto", "Automatic"}};
        if (id == "R4QP01")
            entries.insert(entries.end(), {{"english", "English"}, {"french", "French"}, {"spanish", "Spanish"},
                                           {"german", "German"}, {"italian", "Italian"}});
        else if (id == "R4QJ01") entries.push_back({"japanese", "Japanese"});
        const char* preview = draft_.language.c_str();
        for (const auto& entry : entries) if (draft_.language == entry.value) preview = entry.name;
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##language_combo", preview))
        {
            for (const auto& entry : entries)
                if (ImGui::Selectable(entry.name, draft_.language == entry.value))
                { draft_.language = entry.value; dirty_ = true; }
            ImGui::EndCombo();
        }
    }

    void DisplayPage()
    {
        PageHeader("Display", "How the game window opens and how sharply the game is drawn.");
        const auto run = EffectiveLaunch().settings;
        if (options_.launch.size || options_.launch.fullscreen || options_.launch.aspect)
        {
            const std::string text = "This run: " + std::to_string(run.width) + " x " + std::to_string(run.height) + ", "
                + run.aspect + (run.fullscreen ? ", fullscreen" : ", windowed") + " (command line, not saved)";
            Chip(text.c_str(), color::info, Icon::Info);
            ImGui::Dummy({0, Dp(2)});
        }

        BeginCard("##window", "Window", nullptr, Icon::Display);
        if (BeginRows("##window_rows"))
        {
            Row("Mode", "Fullscreen covers the whole display without changing its resolution.");
            int mode = draft_.fullscreen ? 1 : 0;
            if (Segmented("##mode", &mode, {"Windowed", "Fullscreen"}, ImGui::GetContentRegionAvail().x))
            { draft_.fullscreen = mode == 1; dirty_ = true; }

            Row("Window size", "Used in windowed mode. You can still resize the window while playing.");
            WindowSizeControl();

            Row("Start on display", displays_.size() > 1 ? "Where the game window opens."
                                                          : "Only one display is connected.");
            DisplayChoice();
            EndRows();
        }
        EndCard();

        BeginCard("##picture", "Picture", nullptr, Icon::Sliders);
        if (BeginRows("##picture_rows"))
        {
            Row("Aspect ratio", "16:9 uses the game's widescreen mode, 4:3 its original layout. Automatic picks by window shape at start.");
            const char* values[] = {"auto", "4:3", "16:9"};
            int aspect = -1;
            for (int i = 0; i < 3; ++i) if (draft_.aspect == values[i]) aspect = i;
            if (Segmented("##aspect", &aspect, {"Automatic", "4:3", "16:9"}, ImGui::GetContentRegionAvail().x))
            { draft_.aspect = values[aspect]; dirty_ = true; }
            if (aspect < 0)
                TextWrappedColored(CurrentFonts().caption, color::warning,
                                   ("Saved value " + draft_.aspect + " is not supported by the game. Choose one above.").c_str());

            Row("Sharpness", "Clean and Sharp draw the game straight into the window without the TV flicker "
                "filter; Sharp keeps pixels square. Soft is the original TV picture.");
            const char* pictures[] = {"soft", "clean", "sharp"};
            int picture = 2;
            for (int i = 0; i < 3; ++i) if (draft_.picture == pictures[i]) picture = i;
            if (Segmented("##sharpness", &picture, {"Soft (TV)", "Clean", "Sharp"}, ImGui::GetContentRegionAvail().x))
            { draft_.picture = pictures[picture]; dirty_ = true; }

            Row("3D resolution", "Window renders the game at your window's own resolution, the sharpest picture. "
                "Native is the Wii's 640 x 448; 2x-4x are fixed multiples for slower graphics cards.");
            const char* resolutions[] = {"window", "native", "2x", "3x", "4x"};
            int resolution = 0;
            for (int i = 0; i < 5; ++i) if (draft_.resolution == resolutions[i]) resolution = i;
            if (Segmented("##resolution", &resolution, {"Window", "Native", "2x", "3x", "4x"},
                          ImGui::GetContentRegionAvail().x))
            { draft_.resolution = resolutions[resolution]; dirty_ = true; }

            Row("Antialiasing", "Smooths the jagged edges of 3D shapes (4x multisampling). Costs some GPU time.");
            bool antialiasing = draft_.antialiasing == "4x";
            if (Toggle("##antialiasing", &antialiasing))
            {
                draft_.antialiasing = antialiasing ? "4x" : "off";
                dirty_ = true;
            }

            Row("VSync", "Synchronizes frames with your display to prevent tearing.");
            if (Toggle("##vsync", &draft_.vsync)) dirty_ = true;

            Row("Frame rate in title", "Shows the game's frames per second in the window title.");
            if (Toggle("##fps", &draft_.show_fps)) dirty_ = true;
            EndRows();
        }
        EndCard();
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
                for (std::size_t n = GamepadActionC; n < GamepadActionCount; ++n)
                    row(PadBindingLabel(bindings[n], type), kGamepadActions[n].label);
                row(swap ? "Left stick" : "Right stick", "Pointer");
            });
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
        // Sizes are logical units like the game window's (points on a Retina
        // Mac), so only those that fit the chosen display are offered.
        struct Group { const char* title; std::vector<std::array<int, 2>> sizes; };
        static const std::array<Group, 4> groups{{
            // The Wii picture is 640 x 448 (480p); at 3D resolution Native,
            // whole multiples of it stay sharpest.
            {"Wii output (480p and multiples)", {{640, 480}, {854, 480}, {1280, 960}, {1708, 960}, {1920, 1440}, {2562, 1440}}},
            {"16:9", {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}}},
            {"16:10", {{1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1200}, {2560, 1600}}},
            {"4:3", {{800, 600}, {1024, 768}, {1440, 1080}, {1600, 1200}}},
        }};
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (draft_.monitor > 0 && draft_.monitor <= int(display_ids_.size())) display = display_ids_[size_t(draft_.monitor - 1)];
        SDL_Rect usable{};
        const bool known = display && SDL_GetDisplayUsableBounds(display, &usable);
        auto fits = [&](int w, int h) {
            if (!known) return true;
            const auto fit = platform::FitWindowSize(w, h, usable.w, usable.h);
            return fit.width == w && fit.height == h;
        };
        const auto largest = known ? platform::FitWindowSize(16000, 9000, usable.w, usable.h)
                                   : platform::WindowSize{1280, 720};
        auto aspect = [](int w, int h) {
            const double ratio = double(w) / h;
            return std::abs(ratio - 16.0 / 9) < 0.01 ? "   (16:9)" : std::abs(ratio - 16.0 / 10) < 0.01 ? "   (16:10)"
                 : std::abs(ratio - 4.0 / 3) < 0.01 ? "   (4:3)" : "";
        };
        auto selected = [&](int w, int h) { return draft_.width == w && draft_.height == h; };

        const float width = ImGui::GetContentRegionAvail().x;
        bool custom = !selected(largest.width, largest.height);
        for (const auto& group : groups)
            for (const auto& size : group.sizes) if (selected(size[0], size[1])) custom = false;
        std::string preview = std::to_string(draft_.width) + " x " + std::to_string(draft_.height)
            + aspect(draft_.width, draft_.height) + (custom ? "  (custom)" : "");
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##size", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            const std::string fill = "Largest that fits: " + std::to_string(largest.width) + " x "
                + std::to_string(largest.height);
            if (ImGui::Selectable(fill.c_str(), selected(largest.width, largest.height)))
            { draft_.width = largest.width; draft_.height = largest.height; dirty_ = true; }
            for (const auto& group : groups)
            {
                bool any = false;
                for (const auto& size : group.sizes) any |= fits(size[0], size[1]);
                if (!any) continue;
                ImGui::SeparatorText(group.title);
                for (const auto& size : group.sizes)
                {
                    if (!fits(size[0], size[1])) continue;
                    const std::string label = std::to_string(size[0]) + " x " + std::to_string(size[1])
                        + aspect(size[0], size[1]) + "##" + group.title;
                    if (ImGui::Selectable(label.c_str(), selected(size[0], size[1])))
                    { draft_.width = size[0]; draft_.height = size[1]; dirty_ = true; }
                }
            }
            ImGui::EndCombo();
        }
        int dimensions[] = {draft_.width, draft_.height};
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputInt2("##custom_size", dimensions))
        {
            draft_.width = std::clamp(dimensions[0], 320, 16384);
            draft_.height = std::clamp(dimensions[1], 240, 16384);
            dirty_ = true;
        }
        if (!fits(draft_.width, draft_.height))
        {
            const auto shrunk = platform::FitWindowSize(draft_.width, draft_.height, usable.w, usable.h);
            const std::string note = "Larger than this display; the game opens it at " + std::to_string(shrunk.width)
                + " x " + std::to_string(shrunk.height) + ".";
            TextWrappedColored(CurrentFonts().caption, color::warning, note.c_str());
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
        PageHeader("Audio", "Output volume for the game. Music, sound effect and voice levels are part of the game's own Options menu.");
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
    }

    void KeyRow(const char* button, std::initializer_list<const char*> keys, const char* alternative = nullptr,
                const char* note = nullptr)
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
        bool first = true;
        for (const char* key : keys)
        {
            if (!first) ImGui::SameLine(0, Dp(6));
            KeyCap(key);
            first = false;
        }
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
                wii_status_ = wii_probe_.Result();
                wii_known_ = true;
                wii_next_probe_ns_ = now + 2000000000ull;
            }
        }
        else if (now >= wii_next_probe_ns_)
            wii_probe_.Start(250);
    }

    struct CalibrationTarget
    {
        const char* name;
        float x, y; // 0..1 across the game picture
    };
    static constexpr CalibrationTarget kCalibrationTargets[5] = {
        {"the centre", 0.5f, 0.5f}, {"the top-left corner", 0.1f, 0.1f}, {"the top-right corner", 0.9f, 0.1f},
        {"the bottom-right corner", 0.9f, 0.9f}, {"the bottom-left corner", 0.1f, 0.9f}};

    void StartCalibration(int slot)
    {
        wii_probe_.Close();
        if (!live_.Open(slot))
        {
            Notice("Wii Remote " + std::to_string(slot + 1) + " did not answer. Press a button on it and try again.",
                   NoticeKind::Warning);
            return;
        }
        calibration_ = {};
        calibration_.active = true;
        calibration_.slot = slot;
        // Calibrate on the display the game starts on, filled like the game's
        // fullscreen picture.
        int count = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&count);
        if (ids && draft_.monitor > 0 && draft_.monitor <= count)
        {
            const int position = int(SDL_WINDOWPOS_CENTERED_DISPLAY(ids[draft_.monitor - 1]));
            SDL_SetWindowPosition(window_, position, position);
        }
        SDL_free(ids);
        SDL_SetWindowFullscreen(window_, true);
    }

    void EndCalibration()
    {
        live_.Close();
        calibration_.active = false;
        SDL_SetWindowFullscreen(window_, false);
        wii_next_probe_ns_ = 0;
    }

    void DrawCalibration()
    {
        auto& c = calibration_;
        live_.Poll();
        const auto& io = ImGui::GetIO();
        const ImVec2 size = io.DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##calibration", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled({0, 0}, size, Col(color::window));
        // The game picture fills the screen at the game's aspect ratio.
        const float aspect = draft_.aspect == "4:3" ? 4.0f / 3.0f
            : draft_.aspect == "16:9" ? 16.0f / 9.0f : size.x / std::max(1.0f, size.y);
        float width = size.x, height = size.x / aspect;
        if (height > size.y) { height = size.y; width = size.y * aspect; }
        const ImVec2 origin{(size.x - width) * 0.5f, (size.y - height) * 0.5f};
        draw->AddRect(origin, {origin.x + width, origin.y + height}, Col(color::border), 0, 0, Dp(1));
        for (std::size_t n = 0; n < 5; ++n)
        {
            const auto& target = kCalibrationTargets[n];
            const ImVec2 at{origin.x + target.x * width, origin.y + target.y * height};
            if (n < c.step)
                DrawIcon(draw, Icon::Check, at, Dp(26), Col(color::accent));
            else if (n == c.step)
            {
                const float pulse = 0.5f + 0.5f * std::sin(float(SDL_GetTicks()) * 0.006f);
                draw->AddCircle(at, Dp(34) + Dp(8) * pulse, Col(color::accent, 0.45f), 0, Dp(2));
                DrawIcon(draw, Icon::Target, at, Dp(48), Col(color::accent));
            }
            else
                DrawIcon(draw, Icon::Target, at, Dp(24), Col(color::dim));
        }

        // Where the remote's camera sees the DolphinBar.
        int visible = 0;
        for (const auto& object : live_.Dots()) visible += object.size != 0;
        const auto pair = platform::TrackWiimotePair(c.pair, live_.Dots());
        const bool in_view = pair.has_value() && visible >= 2;
        const auto& target = kCalibrationTargets[std::min<std::size_t>(c.step, 4)];
        const auto& fonts = CurrentFonts();
        const auto centred = [&](ImFont* font, float y, ImU32 ink, const std::string& text) {
            const ImVec2 extent = TextSize(font, text.c_str());
            DrawLabel(draw, font, {(size.x - extent.x) * 0.5f, y}, ink, text.c_str());
        };
        const float text_y = origin.y + height * 0.62f;
        centred(fonts.title, text_y, Col(color::text),
                "Wii Remote " + std::to_string(c.slot + 1) + ": aim at " + target.name + " and press A");
        centred(fonts.label, text_y + Dp(48), Col(in_view ? color::accent : color::warning),
                in_view ? "DolphinBar in view" : "The Wii Remote can't see the DolphinBar");
        if (!c.message.empty()) centred(fonts.body, text_y + Dp(80), Col(color::warning), c.message);
        centred(fonts.caption, origin.y + height - Dp(40), Col(color::muted),
                "Target " + std::to_string(c.step + 1) + " of 5   -   hold the remote as you play   -   B or Esc cancels");
        ImGui::End();

        const std::uint16_t buttons = live_.Buttons();
        const std::uint16_t pressed = buttons & ~c.buttons;
        c.buttons = buttons;
        const int slot = c.slot;
        if ((pressed & 0x0004) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            EndCalibration();
            Notice("Calibration cancelled; Wii Remote " + std::to_string(slot + 1) + " keeps its previous setting.",
                   NoticeKind::Info);
            return;
        }
        if (!live_.Connected())
        {
            if (!c.lost_since_ns) c.lost_since_ns = SDL_GetTicksNS();
            else if (SDL_GetTicksNS() - c.lost_since_ns > 3000000000ull)
            {
                EndCalibration();
                Notice("Wii Remote " + std::to_string(slot + 1) + " stopped answering; calibration cancelled.",
                       NoticeKind::Warning);
            }
            return;
        }
        c.lost_since_ns = 0;
        if (!(pressed & 0x0008)) return;
        if (!in_view)
        {
            c.message = "Move back a little or aim closer to the screen, then press A again.";
            return;
        }
        c.samples.push_back({(*pair)[0], (*pair)[1], target.x, target.y});
        c.message.clear();
        if (++c.step < 5) return;
        const auto fit = platform::FitWiimoteCalibration(c.samples);
        if (!fit)
        {
            c.samples.clear();
            c.step = 0;
            c.message = "Those aims did not line up. Let's start again from the centre.";
            return;
        }
        draft_.remote_calibration[slot] = platform::FormatWiimoteCalibration(*fit);
        dirty_ = true;
        EndCalibration();
        Notice("Wii Remote " + std::to_string(slot + 1) + " calibrated. Save to keep it.", NoticeKind::Success);
    }

    // controls.player1-4: keyboard & mouse, a Wii Remote in a DolphinBar slot
    // or off. Players fill in order and each device plays once. Without a
    // DolphinBar the game makes keyboard & mouse player 1, alone.
    void PlayersCard()
    {
        PollWiimotes();
        const auto& status = wii_status_;
        const bool bar = status.dolphinbar;
        auto& players = draft_.players;
        const char* subtitle = !wii_known_ ? "Looking for a DolphinBar..."
            : status.dolphinbar_other_mode ? "The DolphinBar is in a mouse or gamepad mode. Press its MODE button until light 4 is on."
            : bar ? "Players fill in order and each controller plays once. Changes apply when the game starts."
            : "Players fill in order: keyboard & mouse and gamepads. Connect a Mayflash DolphinBar in mode 4 for Wii Remotes.";
        BeginCard("##players", "Players", subtitle, Icon::Gamepad);
        if (bar)
            Chip("DolphinBar", color::accent, Icon::Check);
        else if (status.dolphinbar_other_mode)
            Chip("DolphinBar: switch to mode 4", color::warning, Icon::Warning);

        const auto connected = [&](int slot) -> const platform::WiimoteHidRemote* {
            for (const auto& remote : status.remotes)
                if (remote.slot == slot) return &remote;
            return nullptr;
        };
        const auto name = [&](const std::string& device) {
            if (device == "keyboard") return std::string("Keyboard & mouse");
            if (device == "off") return std::string("Off");
            const int slot = device.back() - '1';
            if (device.rfind("gamepad", 0) == 0)
                return slot < int(gamepad_names_.size()) ? "Gamepad " + std::to_string(slot + 1) + ": " + gamepad_names_[slot]
                                                         : "Gamepad " + std::to_string(slot + 1) + "  (not connected)";
            if (!bar) return "Wii Remote " + std::to_string(slot + 1) + "  (needs the DolphinBar)";
            const auto* remote = connected(slot);
            return "Wii Remote " + std::to_string(slot + 1)
                + (!remote ? "  (not connected)" : remote->nunchuk ? " + Nunchuk" : "");
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
                    for (int slot = 0; slot < 4; ++slot)
                    {
                        const auto device = "remote" + std::to_string(slot + 1);
                        if ((bar && connected(slot)) || players[n] == device) option(device);
                    }
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
                const int slot = shown.rfind("remote", 0) == 0 ? shown.back() - '1' : -1;
                const auto* remote = slot >= 0 && bar ? connected(slot) : nullptr;
                ImGui::TableSetColumnIndex(3);
                if (remote) BatteryIcon(remote->battery);
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
                if (remote)
                {
                    const bool calibrated = draft_.remote_calibration[slot] != "none";
                    if (SecondaryButton("##calibrate", "", Dp(36, 30), true, Icon::Target)) StartCalibration(slot);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(calibrated ? "Pointer calibrated. Click to calibrate again."
                                                     : "Calibrate the pointer: aim at five targets");
                    if (calibrated)
                    {
                        ImGui::SameLine(0, Dp(6));
                        if (SecondaryButton("##clear", "", Dp(36, 30), true, Icon::Error))
                        {
                            draft_.remote_calibration[slot] = "none";
                            dirty_ = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear the pointer calibration");
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        // Single player on a Wii Remote or gamepad only.
        if (players[0] != "keyboard" && players[1] == "off" && BeginRows("##mouse_rows"))
        {
            Row("Mouse pointer", "The mouse also points for player 1: left click is A, right click is B. "
                "The controller keeps working. Only when player 1 plays alone.");
            if (Toggle("##mouse_pointer", &draft_.mouse_pointer)) dirty_ = true;
            EndRows();
        }
        if ((bar || draft_.sensor_bar != "bottom") && BeginRows("##sensor_rows"))
        {
            Row("Sensor bar", "Where your DolphinBar sits. The game aims the Wii Remote pointer from it.");
            int position = draft_.sensor_bar == "top" ? 0 : 1;
            if (Segmented("##sensor_bar", &position, {"Above the screen", "Below the screen"},
                          ImGui::GetContentRegionAvail().x))
            {
                draft_.sensor_bar = position == 0 ? "top" : "bottom";
                dirty_ = true;
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
        capture_action_ = capture_slot_ = -1;
    }

    void CloseKeyboardView()
    {
        keyboard_view_ = false;
        capture_action_ = capture_slot_ = -1;
        if (dirty_ && config_ok_) Save();
    }

    platform::KeyBindings DraftBindings() const { return platform::ParseKeyBindings(draft_.keys); }

    // A key pressed while a keycap waits: it takes that slot, moving it from
    // another action unless it is that action's only key.
    void CaptureKey(SDL_Scancode code)
    {
        const int action = capture_action_, slot = capture_slot_;
        capture_action_ = capture_slot_ = -1;
        if (action < 0) return;
        const std::string name = SDL_GetScancodeName(code);
        if (code == platform::kScreenshotKey)
        {
            Notice(name + " takes screenshots and cannot be assigned.", NoticeKind::Warning);
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
                if (binding[1 - k] == SDL_SCANCODE_UNKNOWN)
                {
                    Notice(name + " is the only key of " + kKeyActions[other].label
                               + ". Give that action another key first.", NoticeKind::Warning);
                    return;
                }
                binding = {binding[1 - k], SDL_SCANCODE_UNKNOWN};
                moved = int(other);
            }
        }
        mine[slot] = code;
        if (mine[0] == SDL_SCANCODE_UNKNOWN) mine = {mine[1], SDL_SCANCODE_UNKNOWN};
        draft_.keys[action] = platform::FormatKeyBinding(mine);
        if (moved >= 0) draft_.keys[moved] = platform::FormatKeyBinding(bindings[moved]);
        dirty_ = true;
        Notice(name + " now presses " + kKeyActions[action].label
                   + (moved >= 0 ? std::string(" (moved from ") + kKeyActions[moved].label + ")" : std::string()) + ".",
               NoticeKind::Info);
    }

    void KeyboardView()
    {
        if (SecondaryButton("##back", "Players", Dp(124, 36), true, Icon::Back)) { CloseKeyboardView(); return; }
        ImGui::Dummy(Dp(0, 6));
        PageHeader("Keyboard & mouse", "Click a key to change it; each action takes up to two keys. "
                   "Changes are saved when you go back.");
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
                        const SDL_Scancode code = bindings[n][slot];
                        if (slot == 1 && code == SDL_SCANCODE_UNKNOWN && bindings[n][0] == SDL_SCANCODE_UNKNOWN) break;
                        const bool waiting = capture_action_ == int(n) && capture_slot_ == slot;
                        const char* label = waiting ? "Press a key..." : code != SDL_SCANCODE_UNKNOWN
                            ? SDL_GetScancodeName(code) : "+";
                        if (slot) ImGui::SameLine(0, Dp(6));
                        ImGui::PushID(slot);
                        if (KeyCapButton("##key", label, waiting))
                        {
                            clicked = true;
                            capture_action_ = waiting ? -1 : int(n);
                            capture_slot_ = waiting ? -1 : slot;
                        }
                        if (ImGui::IsItemHovered() && !waiting)
                            ImGui::SetTooltip(code != SDL_SCANCODE_UNKNOWN ? "Click, then press the new key"
                                                                           : "Add a second key");
                        ImGui::PopID();
                    }
                    if (bindings[n][1] != SDL_SCANCODE_UNKNOWN)
                    {
                        ImGui::SameLine(0, Dp(6));
                        if (SecondaryButton("##remove", "", Dp(30, 30), true, Icon::Error))
                        {
                            draft_.keys[n] = platform::FormatKeyBinding({bindings[n][0], SDL_SCANCODE_UNKNOWN});
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
        BeginCard("##mouse_keys", "Mouse & more", nullptr, Icon::Mouse);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
        if (ImGui::BeginTable("##fixed_keys", 2))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch);
            KeyRow("Pointer", {"Mouse"}, "left click is A, right click is B", "Point at menu items");
            KeyRow("Screenshot", {"P"}, nullptr, "Saved in the screenshots folder");
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
    // mscharged.ini, each made for one kind of controller; each gamepad plays
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

    // Gamepad presses while an input waits for one: a button, or a trigger
    // pressed past half its travel. Escape stops waiting.
    void GamepadCaptureEvent(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                            || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        {
            auto& held = trigger_held_[event.gaxis.which][event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
            const bool pressed = event.gaxis.value > platform::kGamepadTriggerPressed;
            if (pressed && !held && pad_capture_action_ >= 0)
                CaptureGamepadInput(platform::kGamepadTriggerBase + event.gaxis.axis);
            held = pressed;
        }
        if (pad_capture_action_ < 0) return;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) CaptureGamepadInput(event.gbutton.button);
        else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
            pad_capture_action_ = pad_capture_slot_ = -1;
    }

    // A press while an input waits: it takes that slot, moving it from another
    // action of the profile unless it is that action's only input.
    void CaptureGamepadInput(platform::GamepadInput input)
    {
        const int action = pad_capture_action_, slot = pad_capture_slot_;
        pad_capture_action_ = pad_capture_slot_ = -1;
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
                    ((builtin ? std::string("Built-in, cannot be changed; New makes an editable copy. ") : std::string())
                     + (users.empty() ? "No gamepad plays with this profile." : "Used by " + users + "."))
                        .c_str());
        ImGui::Dummy(Dp(0, 4));
        if (SecondaryButton("##new_profile", "New", Dp(110, 36), profiles.size() < kMaxGamepadProfiles, Icon::Copy))
        {
            auto copy = ShownProfile(edit_profile_);
            copy.name = NewProfileName();
            copy.controller = PageKind();
            profiles.push_back(copy);
            EditProfile(int(profiles.size()) - 1);
            dirty_ = true;
            Notice(copy.name + " starts as a copy of the profile you were editing.", NoticeKind::Info);
        }
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
                   "Pick a profile per gamepad, here or on the Players list. To change a button, click it below "
                   "and press the new one on the controller. Changes are saved when you go back.");
        if (edit_profile_ >= int(draft_.pad_profiles.size())) edit_profile_ = -1;
        ProfileCard();
        // The built-in Default is shown read-only; New makes an editable copy.
        const bool builtin = edit_profile_ < 0;
        Settings::PadProfile scratch = ShownProfile(edit_profile_);
        auto& profile = builtin ? scratch : draft_.pad_profiles[std::size_t(edit_profile_)];
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
                        if (slot == 1 && (bindings[n][0] == platform::kGamepadNone || builtin)) break;
                        const bool waiting = pad_capture_action_ == int(n) && pad_capture_slot_ == slot;
                        const std::string label = waiting ? "Press a button..."
                            : input != platform::kGamepadNone ? PadInputLabel(input, type)
                            : slot == 0 ? "Not set" : "+";
                        if (slot) ImGui::SameLine(0, Dp(6));
                        ImGui::PushID(slot);
                        if (KeyCapButton("##input", label.c_str(), waiting))
                        {
                            clicked = true;
                            pad_capture_action_ = waiting ? -1 : int(n);
                            pad_capture_slot_ = waiting ? -1 : slot;
                        }
                        if (ImGui::IsItemHovered() && !waiting)
                            ImGui::SetTooltip(input != platform::kGamepadNone
                                                  ? "Click, then press the new button on the controller"
                                                  : slot == 0 ? "No button presses this. Click, then press one."
                                                              : "Add a second button");
                        ImGui::PopID();
                    }
                    if (bindings[n][0] != platform::kGamepadNone && !builtin)
                    {
                        // Removes the second button, or the only one (no button then).
                        const bool two = bindings[n][1] != platform::kGamepadNone;
                        ImGui::SameLine(0, Dp(6));
                        if (SecondaryButton("##remove", "", Dp(30, 30), true, Icon::Error))
                        {
                            profile.inputs[n] = platform::FormatGamepadBinding(
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
        ImGui::BeginDisabled(builtin);
        const std::string remote_title = "Wii Remote buttons: " + profile.name;
        BeginCard("##pad_remote", remote_title.c_str(),
                  "Each Wii Remote button, and the controller button that presses it.", Icon::Gamepad);
        rows(GamepadActionA, GamepadActionC);
        EndCard();
        BeginCard("##pad_nunchuk", "Nunchuk & motion",
                  "The Nunchuk stick and pointer on the controller's sticks, the Nunchuk buttons and the shakes.",
                  Icon::Gamepad);
        const bool swap = profile.swap_sticks;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
        if (ImGui::BeginTable("##pad_sticks", 2))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
            ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthStretch);
            KeyRow("Nunchuk stick", {swap ? "Right stick" : "Left stick"}, nullptr, "Move, aim");
            KeyRow("Pointer", {swap ? "Left stick" : "Right stick"}, nullptr, "Point at menu items");
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        if (BeginRows("##pad_swap_rows"))
        {
            Row("Swap sticks", "Nunchuk stick on the right stick, pointer on the left.");
            bool swapped = swap;
            if (Toggle("##swap_sticks", &swapped))
            {
                profile.swap_sticks = swapped;
                dirty_ = true;
            }
            EndRows();
        }
        rows(GamepadActionC, GamepadActionCount);
        // Defaults of the controller shown above.
        const auto defaults = platform::DefaultGamepadInputs(PageFamily());
        const bool changed = platform::ParseGamepadBindings(profile.inputs) != platform::ParseGamepadBindings(defaults)
            || swap;
        if (SecondaryButton("##reset_pad", "Reset to defaults", Dp(180, 36), changed, Icon::Refresh))
        {
            profile.inputs = defaults;
            profile.swap_sticks = false;
            pad_capture_action_ = pad_capture_slot_ = -1;
            dirty_ = true;
        }
        EndCard();
        ImGui::EndDisabled();
        // A click anywhere else stops waiting for a press.
        if (pad_capture_action_ >= 0 && !clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            pad_capture_action_ = pad_capture_slot_ = -1;
    }

    void ControlsMain()
    {
        PageHeader("Controls", "Play with keyboard and mouse, an Xbox, PlayStation or other gamepad, or a Wii Remote with Nunchuk. Keep the game window focused.");
        PlayersCard();

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

    void AdvancedPage(bool& quit)
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
            EndRows();
        }
        EndCard();

        BeginCard("##graphics", "Graphics", nullptr, Icon::Display);
        if (BeginRows("##graphics_rows"))
        {
#if defined(__APPLE__)
            const char* api = "Metal";
#elif defined(_WIN32)
            const char* api = "Direct3D 12";
#else
            const char* api = "Vulkan";
#endif
            Row("Graphics API", "Chosen for your system by this build.");
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(4));
            Chip(api, color::info);
            Row("API validation", "Checks every graphics call for errors. Useful for bug reports, but slower.");
            if (Toggle("##validation", &draft_.graphics_validation)) dirty_ = true;
            EndRows();
        }
        EndCard();

        BeginCard("##diagnostics", "Diagnostics", nullptr, Icon::Info);
        if (BeginRows("##diagnostic_rows"))
        {
            Row("Log detail", "Which messages reach the console: errors, warnings, info or debug.");
            const char* values[] = {"error", "warning", "info", "debug"};
            int level = 2;
            for (int i = 0; i < 4; ++i) if (draft_.log_level == values[i]) level = i;
            if (Segmented("##log_level", &level, {"Errors", "Warnings", "Info", "Debug"}, ImGui::GetContentRegionAvail().x))
            { draft_.log_level = values[level]; dirty_ = true; }

            Row("Verbose console", "Also prints development traces and the game's own debug output. Useful for bug reports.");
            if (Toggle("##verbose_console", &draft_.verbose_console)) dirty_ = true;

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

#if defined(MSCHARGED_HAS_ORIGINAL_CREDITS) || defined(MSCHARGED_HAS_GAME_STARTUP)
        BeginCard("##developer", "Developer tests", "Run a single original scene for testing. Settings are used without saving.", Icon::Play);
        if (BeginRows("##developer_rows"))
        {
            const bool ready = DiscReady() && SupportedRelease(*check_.info);
#ifdef MSCHARGED_HAS_ORIGINAL_CREDITS
            Row("Original Credits", "Runs the original Credits scene with its movie and music.");
            if (SecondaryButton("credits_test", "Run Credits", {ImGui::GetContentRegionAvail().x, Dp(40)}, ready, Icon::Play))
            { credits_launch_ = EffectiveLaunch(); quit = true; }
#endif
#ifdef MSCHARGED_HAS_GAME_STARTUP
            Row("Original initialization", "Runs the original startup prefix and reports its first missing service.");
            if (SecondaryButton("startup_test", "Run startup", {ImGui::GetContentRegionAvail().x, Dp(40)}, ready, Icon::Play))
            { startup_launch_ = EffectiveLaunch(); quit = true; }
#endif
            EndRows();
        }
        EndCard();
#else
        (void)quit;
#endif

        BeginCard("##reset", "Reset", nullptr, Icon::Refresh);
        if (BeginRows("##reset_rows"))
        {
            Row("Restore defaults", "Resets every setting except your disc. Save to keep the result.");
            if (SecondaryButton("defaults", "Restore defaults", {ImGui::GetContentRegionAvail().x, Dp(40)}, config_ok_, Icon::Refresh))
            {
                const auto disc = draft_.disc;
                draft_ = {};
                draft_.disc = disc;
                dirty_ = true;
                Notice("Defaults restored. Save to keep them.", NoticeKind::Info);
            }
            EndRows();
        }
        EndCard();
    }

    void AboutPage()
    {
        PageHeader("About", "Mario Strikers Charged - native PC port.");
        BeginCard("##build", "This build", nullptr, Icon::Info);
        if (BeginRows("##build_rows"))
        {
            auto value = [&](const char* label, const std::string& text) {
                Row(label);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(2));
                TextWrappedColored(CurrentFonts().body, color::muted, text.c_str());
            };
            value("Version", build::version);
            value("Configuration", MSCHARGED_BUILD_CONFIG);
            const int sdl = SDL_GetVersion();
            value("Platform", std::string(SDL_GetPlatform()) + "  -  SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl)) + "."
                + std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(sdl)));
            const char* renderer = SDL_GetRendererName(renderer_);
            char scale[96];
            std::snprintf(scale, sizeof scale, "%s  -  interface x%.2f  -  pixel density x%.2f",
                          renderer ? renderer : "unknown renderer", metrics_.ui, metrics_.density);
            value("Launcher display", scale);
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
            value("Game runtime", "Included: original game code compiled for this system.");
#else
            value("Game runtime", "Not included in this launcher-only build.");
#endif
            EndRows();
        }
        EndCard();

        BeginCard("##project", "Mario Strikers Charged Decompilation Project", nullptr, Icon::Disc);
        TextWrappedColored(CurrentFonts().body, color::text,
            "This port compiles the reconstructed C/C++ source of Mario Strikers Charged to native code. "
            "The original game logic runs unchanged; the port provides the platform underneath it: graphics, audio, "
            "input, files and timing.");
        ImGui::Dummy({0, Dp(2)});
        TextWrappedColored(CurrentFonts().caption, color::muted,
            "You need your own copy of the game. Unofficial project, not affiliated with Nintendo or Next Level Games.");
        ImGui::Dummy({0, Dp(4)});
        if (SecondaryButton("decomp_link", "Decompilation project", Dp(230, 40), true, Icon::GitHub))
            SDL_OpenURL("https://github.com/yannicksuter/mscharged-decomp");
        EndCard();

        BeginCard("##aurora", "Powered by Aurora", nullptr, Icon::Display);
        if (aurora_logo_)
        {
            const float width = Dp(180);
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(aurora_logo_)),
                         {width, width * aurora_logo_height_ / aurora_logo_width_});
            ImGui::Dummy({0, Dp(4)});
        }
        TextWrappedColored(CurrentFonts().body, color::text,
            "Aurora by Luke Street provides the GameCube/Wii platform layer the game runs on: GX graphics on "
            "modern GPUs, plus windowing, input and system services.");
        ImGui::Dummy({0, Dp(4)});
        if (SecondaryButton("aurora_link", "Aurora on GitHub", Dp(230, 40), true, Icon::GitHub))
            SDL_OpenURL("https://github.com/encounter/aurora");
        EndCard();

        BeginCard("##thanks", "Special thanks", nullptr, Icon::Check);
        TextWrappedColored(CurrentFonts().body, color::text,
            "To the contributors of the decompilation, who helped bring it over the last miles:");
        ImGui::Dummy({0, Dp(2)});
        {
            // GitHub accounts; each button opens the profile.
            const char* contributors[] = {"Jasu14", "vZylev", "GoldenPalazzo"};
            for (int i = 0; i < 3; ++i)
            {
                if (i) ImGui::SameLine(0, Dp(8));
                const std::string url = std::string("https://github.com/") + contributors[i];
                ImGui::PushID(i);
                if (SecondaryButton("##contributor", contributors[i], Dp(170, 36), true, Icon::Link))
                    SDL_OpenURL(url.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", url.c_str());
                ImGui::PopID();
            }
        }
        EndCard();

        BeginCard("##licenses", "Credits & licenses", nullptr, Icon::Check);
        TextWrappedColored(CurrentFonts().caption, color::muted,
            "Original port code, tools and documentation: CC0 1.0. Header artwork: SteamGridDB hero 8926 by Jiquita. "
            "Font: Roboto (Apache 2.0). Built with SDL3 (zlib), Dear ImGui (MIT), Aurora (MIT), Dawn (BSD), "
            "nod and its Rust dependencies, and other libraries listed in LICENSES. Each keeps its own license.");
        EndCard();
    }

    Options options_;
    fs::path base_;
    std::string font_path_;
    bool capture_ = false;
    UiMetrics metrics_{};
    std::optional<ResolvedLaunch> startup_launch_;
    std::optional<ResolvedLaunch> credits_launch_;
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
    // Triggers past half travel per pad, so a held trigger is not a new press.
    std::map<SDL_JoystickID, std::array<bool, 2>> trigger_held_;
    // Wii Remote / DolphinBar detection for the Controls page (non-blocking).
    platform::WiimoteHidProbe wii_probe_;
    platform::WiimoteHidStatus wii_status_{};
    // Wii Remote pointer calibration: fullscreen targets aimed at with the
    // remote, A to set each one, B to cancel.
    struct Calibration
    {
        bool active = false;
        int slot = -1;
        std::size_t step = 0;
        std::vector<platform::WiimoteCalibrationSample> samples;
        platform::WiimotePairTracker pair;
        std::uint16_t buttons = 0;
        std::string message;
        Uint64 lost_since_ns = 0;
    } calibration_;
    platform::WiimoteHidLive live_;
    // Controls: keyboard & mouse keys view, and the keycap waiting for a key.
    bool keyboard_view_ = false;
    int capture_action_ = -1, capture_slot_ = -1;
    // Gamepad profiles view: the profile being edited, its rename field and
    // the input waiting for a gamepad press.
    bool gamepad_view_ = false;
    int edit_profile_ = -1; // -1: the built-in Default
    std::size_t pad_slot_ = 0; // the gamepad the page was opened for
    bool renaming_ = false, rename_focus_ = false, confirm_delete_ = false;
    std::string rename_buffer_;
    int pad_capture_action_ = -1, pad_capture_slot_ = -1;
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
    std::future<DiscCheck> future_;
    std::vector<std::string> displays_;
    std::vector<SDL_DisplayID> display_ids_;
    std::shared_ptr<DialogResult> dialog_ = std::make_shared<DialogResult>();
};
}

int main(int argc, char** argv)
{
    Options options;
    try
    {
    for (int i = 1; i < argc; ++i)
    {
        if (ParseLaunchOption(argc, argv, i, options.launch)) continue;
        const std::string arg = argv[i];
        if (arg == "--version")
        {
            std::cout << "mscharged " << mscharged::build::version << " (" << MSCHARGED_BUILD_CONFIG;
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
            std::cout << "; original frontend runtime)\n";
#else
            std::cout << "; launcher)\n";
#endif
            return 0;
        }
        if (arg == "--help")
        {
            std::cout << "Usage: mscharged [launch settings] [--launcher] [--version]\n" << LaunchOptionsHelp <<
                         "Supplying --disc/--disk starts the original game runtime directly when included in this build.\n"
                         "Use --launcher to open settings; launcher checks and explicit runtime modes keep their selected mode.\n"
                         "Development checks: --smoke-test | --screenshot FILE [--page play|game|display|audio|controls|advanced|about]\n";
#ifdef MSCHARGED_HAS_GAME_STARTUP
            std::cout << "Original startup prototype: --experimental-startup [--config FILE] (not playable)\n";
#endif
#ifdef MSCHARGED_HAS_ORIGINAL_CREDITS
            std::cout << "Original-main Credits test: --experimental-credits [--disk FILE] [--window] (temporary startup omissions)\n";
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
        else if (arg == "--experimental-credits") options.experimental_credits = true;
        else if (arg == "--experimental-frontend") options.experimental_frontend = true;
        else if (arg == "--experimental-options") options.experimental_options = true;
        else if (arg == "--native-send" || arg == "--diagnostic-frame" || arg == "--resize-check") options.credits_arguments = true;
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
    // logs/mscharged.log for player runs (not the development smoke/screenshot checks).
    if (!options.smoke_test && options.screenshot.empty())
        mscharged::platform::StartSessionLog(mscharged::build::version, argc, argv);
    options.config = options.launch.config;
    unsigned runtime_modes = unsigned(options.experimental_startup)
        + unsigned(options.experimental_scene) + unsigned(options.experimental_credits)
        + unsigned(options.experimental_frontend) + unsigned(options.experimental_options);
    const bool launcher_requested = options.launcher || options.smoke_test
        || !options.screenshot.empty() || options.page_selected;
    if (!runtime_modes && options.launch.disc && !launcher_requested)
    {
        options.experimental_frontend = true;
        runtime_modes = 1;
    }
    if (runtime_modes && (runtime_modes > 1 || launcher_requested))
    { std::cerr << "Select one runtime mode; --launcher and capture/smoke/page options select the launcher.\n"; return 2; }
    if (options.credits_arguments && !options.experimental_credits && !options.experimental_frontend && !options.experimental_options)
    { std::cerr << "Frame/resize options require --experimental-credits or --experimental-frontend.\n"; return 2; }
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
            return RunOriginalMainCredits(static_cast<int>(arguments.size()) - 1, arguments.data(),
                nullptr, false, options.experimental_options ?
                    OriginalMainScene::FrontendOptions : OriginalMainScene::FrontendSequence, GameTitle().c_str());
#else
            std::cerr << "Original game runtime is not included in this build. "
                         "A game-enabled build is required for direct disc startup; --launcher opens settings.\n";
            return 2;
#endif
        }
        if (options.experimental_credits)
        {
#ifdef MSCHARGED_HAS_ORIGINAL_CREDITS
            // Preserve all shared launch options and diagnostic frame arguments;
            // remove only the launcher mode selector before the shared driver.
            std::vector<char*> arguments;
            for (int i = 0; i < argc; ++i)
                if (std::string_view(argv[i]) != "--experimental-credits") arguments.push_back(argv[i]);
            arguments.push_back(nullptr);
            return RunOriginalMainCredits(static_cast<int>(arguments.size()) - 1, arguments.data(), nullptr, false,
                OriginalMainScene::Credits, GameTitle().c_str());
#else
            std::cerr << "Original-main Credits is not in this build. Enable MSCHARGED_BUILD_ORIGINAL_CREDITS_DIAGNOSTIC and MSCHARGED_BUILD_LAUNCHER.\n";
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
        std::optional<ResolvedLaunch> startup_launch, credits_launch, frontend_launch;
        int result;
        {
            Launcher launcher;
            result = launcher.Run(std::move(options));
            startup_launch = launcher.StartupLaunch();
            credits_launch = launcher.CreditsLaunch();
            frontend_launch = launcher.FrontendLaunch();
        } // Destroy the launcher/ImGui/SDL session before Aurora initializes.
#ifdef MSCHARGED_HAS_ORIGINAL_CREDITS
        if (credits_launch) return RunOriginalMainCredits(argc, argv, &*credits_launch, true,
            OriginalMainScene::Credits, GameTitle().c_str());
#endif
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
        if (frontend_launch) return RunOriginalMainCredits(argc, argv, &*frontend_launch, true,
            OriginalMainScene::FrontendSequence, GameTitle().c_str());
#endif
#ifdef MSCHARGED_HAS_GAME_STARTUP
        if (startup_launch) return RunGameStartup(argc, argv, *startup_launch);
#endif
        return result;
    }
    catch (const std::exception& e)
    {
        mscharged::platform::ReportFatalError(std::string("Launcher failed: ") + e.what());
        return 1;
    }
}
