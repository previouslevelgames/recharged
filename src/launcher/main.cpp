#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_stdlib.h"

#include "bootstrap/config.h"
#include "mscharged/build_version.h"
#include "platform/disc.h"
#include "platform/path.h"
#ifdef MSCHARGED_HAS_GAME_STARTUP
#include "runtime/startup.h"
#endif
#include "runtime/scene.h"
#include <charconv>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace mscharged;

namespace
{
constexpr const char* pages[] = {"Game", "Display", "Audio", "Controls", "About"};
constexpr const char* page_ids[] = {"game", "display", "audio", "controls", "about"};
const ImVec4 muted{0.61f, 0.65f, 0.72f, 1.0f};
const ImVec4 accent{0.29f, 0.67f, 1.0f, 1.0f};
const ImVec4 good{0.39f, 0.84f, 0.63f, 1.0f};
const ImVec4 warning{1.0f, 0.73f, 0.37f, 1.0f};
const ImVec4 bad{1.0f, 0.43f, 0.44f, 1.0f};

struct Options
{
    fs::path config;
    fs::path screenshot;
    bool smoke_test = false;
    bool experimental_startup = false;
    bool experimental_scene = false;
    bool scene_arguments = false;
    bool page_selected = false;
    SceneOptions scene;
    int page = 0;
};

void Require(bool ok, const char* context)
{
    if (!ok) throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

fs::path DefaultConfig(const fs::path& executable_directory)
{
    if (fs::exists("mscharged.ini")) return fs::absolute("mscharged.ini");
    for (auto directory = executable_directory; !directory.empty(); directory = directory.parent_path())
    {
        if (fs::exists(directory / "mscharged.ini.example")) return directory / "mscharged.ini";
        if (directory == directory.parent_path()) break;
    }
    return executable_directory / "mscharged.ini";
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

bool Choice(const char* label, std::string& selected, std::initializer_list<std::pair<const char*, const char*>> entries)
{
    const char* preview = selected.c_str();
    for (const auto& [value, name] : entries) if (selected == value) preview = name;
    bool changed = false;
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo(label, preview))
    {
        for (const auto& [value, name] : entries)
            if (ImGui::Selectable(name, selected == value)) { selected = value; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

void Paragraph(const char* text, ImVec4 color = muted)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

class Launcher
{
public:
    const fs::path& StartupConfig() const { return startup_config_; }

    ~Launcher()
    {
        if (renderer_ui_ready_) ImGui_ImplSDLRenderer3_Shutdown();
        if (window_ui_ready_) ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        if (gamepad_) SDL_CloseGamepad(gamepad_);
        if (header_) SDL_DestroyTexture(header_);
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
        const auto base = PathFromUtf8(base_path);
        if (options_.config.empty()) options_.config = DefaultConfig(base);
        file_.path = fs::absolute(options_.config);
        Reload();
        page_ = options_.page;

        const bool capture = options_.smoke_test || !options_.screenshot.empty();
        int width = 960, height = 680;
        SDL_Rect available{};
        if (!capture && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &available))
        {
            width = std::min(width, available.w - 48);
            height = std::min(height, available.h - 72);
        }
        window_ = SDL_CreateWindow("Mario Strikers Charged | Launcher", width, height,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        Require(window_ != nullptr, "Cannot create launcher window");
        SDL_SetWindowMinimumSize(window_, 800, 600);
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        Require(renderer_ != nullptr, "Cannot create launcher renderer");
        SDL_SetRenderVSync(renderer_, 1);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        const auto resources = base / "assets/launcher";
        auto surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
            SDL_LoadPNG(PathUtf8(resources / "header.png").c_str()), SDL_DestroySurface);
        Require(surface != nullptr, "Cannot load launcher header; rebuild to restore resources");
        header_ratio_ = float(surface->h) / float(surface->w);
        header_width_ = float(surface->w);
        header_height_ = float(surface->h);
        header_ = SDL_CreateTextureFromSurface(renderer_, surface.get());
        Require(header_ != nullptr, "Cannot create header texture");
        SDL_SetTextureScaleMode(header_, SDL_SCALEMODE_LINEAR);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr; // UI state must not create a public imgui.ini.
        io.LogFilename = nullptr;
        const auto font = PathUtf8(resources / "Roboto-Medium.ttf");
        if (!fs::exists(resources / "Roboto-Medium.ttf"))
            throw std::runtime_error("Missing launcher font; rebuild to restore resources");
        normal_ = io.Fonts->AddFontFromFileTTF(font.c_str(), 17);
        heading_ = io.Fonts->AddFontFromFileTTF(font.c_str(), 24);
        title_ = io.Fonts->AddFontFromFileTTF(font.c_str(), 36);
        io.FontDefault = normal_;
        SetStyle();
        Require(ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_), "Cannot initialize window UI");
        window_ui_ready_ = true;
        Require(ImGui_ImplSDLRenderer3_Init(renderer_), "Cannot initialize UI renderer");
        renderer_ui_ready_ = true;
        RefreshGamepad();
        if (!capture) SDL_ShowWindow(window_);
        if (!draft_.disc.empty() && !options_.smoke_test && config_ok_) CheckDisc();

        bool quit = false;
        unsigned frame = 0;
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
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                    SelectDisc(event.drop.data);
            }
            PollResults();
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (options_.smoke_test) page_ = int(frame / 4) % 5;
            Draw(quit);
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer_, 17, 21, 29, 255);
            SDL_RenderClear(renderer_);
            RenderHeader();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
            if (!options_.screenshot.empty() && frame >= 3 && (!pending_ || frame > 600))
            {
                auto pixels = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
                    SDL_RenderReadPixels(renderer_, nullptr), SDL_DestroySurface);
                Require(pixels != nullptr, "Cannot capture launcher");
                Require(SDL_SavePNG(pixels.get(), PathUtf8(options_.screenshot).c_str()), "Cannot save screenshot");
                quit = true;
            }
            SDL_RenderPresent(renderer_);
            ++frame;
            if (options_.smoke_test && frame >= 20) quit = true;
            const auto elapsed = SDL_GetTicks() - frame_start;
            if (elapsed < 16) SDL_Delay(Uint32(16 - elapsed));
        }
        if (options_.smoke_test) std::cout << "Launcher rendered all five pages and loaded its resources.\n";
        return 0;
    }

private:
    float HeroHeight() const
    {
        const auto size = ImGui::GetIO().DisplaySize;
        return std::min({size.x * header_ratio_, size.y * 0.28f, 190.0f});
    }

    void RenderHeader()
    {
        // SDL's texture path also works around large textured-triangle artifacts
        // in the software renderer. Crop vertically without stretching the art.
        const auto& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
        const float crop_height = header_width_ * HeroHeight() / io.DisplaySize.x;
        const SDL_FRect source{0, (header_height_ - crop_height) * 0.5f, header_width_, crop_height};
        const SDL_FRect destination{0, 0, io.DisplaySize.x * io.DisplayFramebufferScale.x,
                                  HeroHeight() * io.DisplayFramebufferScale.y};
        SDL_RenderTexture(renderer_, header_, &source, &destination);
    }

    void SetStyle()
    {
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowPadding = {14, 14};
        style.FramePadding = {9, 5};
        style.ItemSpacing = {8, 6};
        style.ItemInnerSpacing = {8, 6};
        style.FrameRounding = 4;
        style.ChildRounding = 6;
        style.PopupRounding = 8;
        style.ScrollbarRounding = 8;
        style.WindowBorderSize = 0;
        style.Colors[ImGuiCol_WindowBg] = {0.067f, 0.082f, 0.114f, 1};
        style.Colors[ImGuiCol_ChildBg] = {0.085f, 0.105f, 0.145f, 1};
        style.Colors[ImGuiCol_Text] = {0.91f, 0.93f, 0.97f, 1};
        style.Colors[ImGuiCol_TextDisabled] = muted;
        style.Colors[ImGuiCol_Border] = {0.20f, 0.24f, 0.31f, 1};
        style.Colors[ImGuiCol_FrameBg] = {0.12f, 0.15f, 0.20f, 1};
        style.Colors[ImGuiCol_FrameBgHovered] = {0.18f, 0.23f, 0.31f, 1};
        style.Colors[ImGuiCol_Button] = {0.17f, 0.22f, 0.29f, 1};
        style.Colors[ImGuiCol_ButtonHovered] = {0.24f, 0.34f, 0.46f, 1};
        style.Colors[ImGuiCol_ButtonActive] = {0.22f, 0.43f, 0.65f, 1};
        style.Colors[ImGuiCol_Header] = {0.13f, 0.30f, 0.47f, 1};
        style.Colors[ImGuiCol_HeaderHovered] = {0.16f, 0.28f, 0.40f, 1};
        style.Colors[ImGuiCol_CheckMark] = accent;
        style.Colors[ImGuiCol_SliderGrab] = accent;
        style.Colors[ImGuiCol_NavCursor] = accent;
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
            notice_ = file_.exists ? "Settings loaded." : "Choose a disc image, then save your settings.";
            notice_error_ = false;
        }
        catch (const std::exception& e) { notice_ = e.what(); notice_error_ = true; config_ok_ = false; }
    }

    bool Save()
    {
        try
        {
            SaveConfig(file_, draft_);
            dirty_ = false;
            notice_ = "Settings saved.";
            notice_error_ = false;
            return true;
        }
        catch (const std::exception& e) { notice_ = e.what(); notice_error_ = true; return false; }
    }

    void SelectDisc(const std::string& path)
    {
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
            const auto path = ResolveDiscPath(draft_, file_.path);
            const auto selection = draft_.disc;
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
        static const SDL_DialogFileFilter filters[] = {{"Wii disc images (ISO / RVZ)", "iso;rvz"}};
        {
            std::lock_guard<std::mutex> lock(dialog_->mutex);
            dialog_->active = true;
        }
        SDL_ShowOpenFileDialog(DiscChosen, new std::shared_ptr<DialogResult>(dialog_), window_, filters, 1, nullptr, false);
    }

    void PollResults()
    {
        {
            std::lock_guard<std::mutex> lock(dialog_->mutex);
            if (dialog_->ready)
            {
                dialog_->ready = false;
                if (!dialog_->error.empty()) { notice_ = dialog_->error; notice_error_ = true; }
                else if (!dialog_->path.empty()) SelectDisc(dialog_->path);
            }
        }
        if (pending_ && future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto result = future_.get();
            pending_ = false;
            if (result.selection == draft_.disc) check_ = std::move(result);
        }
    }

    void RefreshGamepad()
    {
        if (gamepad_) { SDL_CloseGamepad(gamepad_); gamepad_ = nullptr; }
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) gamepad_ = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }

    void PageTitle(const char* title, const char* description)
    {
        ImGui::PushFont(heading_);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        Paragraph(description);
    }

    void LanguageSetting()
    {
        // Follow the region branches in the pinned decomp's Game/main.cpp.
        // This remains a saved preference until the game's SCGetLanguage path is wired up.
        const std::string id = check_.info ? check_.info->game_id : "";
        bool supported = draft_.language == "auto";
        if (id == "R4QE01")
        {
            dirty_ |= Choice("Game text language", draft_.language, {{"auto", "Automatic"},
                {"english", "English"}, {"french", "French"}, {"spanish", "Spanish"}});
            supported |= draft_.language == "english" || draft_.language == "french" || draft_.language == "spanish";
        }
        else if (id == "R4QP01")
        {
            dirty_ |= Choice("Game text language", draft_.language, {{"auto", "Automatic"},
                {"english", "English"}, {"french", "French"}, {"spanish", "Spanish"},
                {"german", "German"}, {"italian", "Italian"}});
            supported |= draft_.language != "japanese";
        }
        else if (id == "R4QJ01")
        {
            dirty_ |= Choice("Game text language", draft_.language, {{"auto", "Automatic"}, {"japanese", "Japanese"}});
            supported |= draft_.language == "japanese";
        }
        else
        {
            ImGui::BeginDisabled();
            Choice("Game text language", draft_.language, {{"auto", "Automatic"},
                {"english", "English"}, {"french", "French"}, {"spanish", "Spanish"},
                {"german", "German"}, {"italian", "Italian"}, {"japanese", "Japanese"}});
            ImGui::EndDisabled();
            Paragraph("Check a supported game image to see its text language options.");
        }
        supported |= draft_.language == "auto";
        if (!supported && (id == "R4QE01" || id == "R4QP01" || id == "R4QJ01"))
            Paragraph("The saved language is unavailable for this release. Choose Automatic or a listed language.", warning);
        Paragraph("Menus and on-screen text. Saved for when gameplay is available.");
    }

    void GamePage()
    {
        PageTitle("Your game", "Open your ISO or RVZ directly. No extraction needed.");
        ImGui::TextUnformatted("Disc image");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 98);
        if (ImGui::InputText("##disc", &draft_.disc)) { dirty_ = true; check_ = {}; }
        ImGui::SameLine();
        bool browsing;
        { std::lock_guard<std::mutex> lock(dialog_->mutex); browsing = dialog_->active; }
        ImGui::BeginDisabled(browsing || !config_ok_);
        if (ImGui::Button("Browse...", {90, 0})) Browse();
        ImGui::EndDisabled();
        ImGui::BeginDisabled(pending_ || draft_.disc.empty() || !config_ok_);
        if (ImGui::Button(pending_ ? "Checking..." : "Check disc", {110, 0})) CheckDisc();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(muted, "Or drop an image onto this window.");
        ImGui::BeginChild("disc_status", {0, 96}, ImGuiChildFlags_Borders);
        if (pending_) Paragraph("Reading the disc header and game file table...", accent);
        else if (check_.info)
        {
            const auto& info = *check_.info;
            ImGui::TextColored(good, "Disc read successfully");
            ImGui::Text("%s  |  Revision %u  |  %s  |  %u files", info.game_id.c_str(),
                        unsigned(info.revision), info.format.c_str(), info.file_count);
            if (info.game_id != "R4QE01" || info.revision != 1)
                Paragraph("Different from the current USA revision 1 baseline; compatibility is unverified.", warning);
            else Paragraph("Matches the current USA revision 1 source baseline.");
        }
        else if (!check_.error.empty()) Paragraph(check_.error.c_str(), bad);
        else Paragraph("Choose an image and check that its game data partition can be opened.");
        ImGui::EndChild();
        LanguageSetting();
    }

    void DisplayPage()
    {
        PageTitle("Display", "Game display preferences. These take effect when gameplay is available.");
        const std::string resolution = std::to_string(draft_.width) + " x " + std::to_string(draft_.height);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Resolution", resolution.c_str()))
        {
            for (const auto& size : {ImVec2{640, 480}, ImVec2{1280, 720}, ImVec2{1920, 1080}, ImVec2{2560, 1440}, ImVec2{3840, 2160}})
            {
                const auto label = std::to_string(int(size.x)) + " x " + std::to_string(int(size.y));
                if (ImGui::Selectable(label.c_str(), resolution == label))
                { draft_.width = int(size.x); draft_.height = int(size.y); dirty_ = true; }
            }
            ImGui::EndCombo();
        }
        int dimensions[] = {draft_.width, draft_.height};
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputInt2("Custom width / height", dimensions))
        { draft_.width = dimensions[0]; draft_.height = dimensions[1]; dirty_ = true; }
        dirty_ |= Choice("Aspect ratio", draft_.aspect, {{"auto", "Follow the window"}, {"4:3", "4:3"},
            {"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}});
        bool backend_supported = draft_.backend == "auto";
#if defined(__APPLE__)
        dirty_ |= Choice("Graphics backend", draft_.backend, {{"auto", "Automatic"}, {"metal", "Metal"}});
        backend_supported |= draft_.backend == "metal";
#elif defined(_WIN32)
        dirty_ |= Choice("Graphics backend", draft_.backend, {{"auto", "Automatic"},
            {"d3d12", "Direct3D 12"}, {"vulkan", "Vulkan"}});
        backend_supported |= draft_.backend == "d3d12" || draft_.backend == "vulkan";
#elif defined(__linux__)
        dirty_ |= Choice("Graphics backend", draft_.backend, {{"auto", "Automatic (Vulkan)"}, {"vulkan", "Vulkan"}});
        backend_supported |= draft_.backend == "vulkan";
#else
        dirty_ |= Choice("Graphics backend", draft_.backend, {{"auto", "Automatic"}});
#endif
        backend_supported |= draft_.backend == "auto";
        if (!backend_supported)
            Paragraph("The saved backend is unavailable on this platform. Choose a listed backend.", warning);
        dirty_ |= ImGui::Checkbox("Fullscreen", &draft_.fullscreen);
        ImGui::SameLine(180);
        dirty_ |= ImGui::Checkbox("VSync", &draft_.vsync);
        Paragraph("Applies to the game window. The launcher keeps its own size.");
    }

    void AudioPage()
    {
        PageTitle("Audio", "Set your preferred mix. Audio playback is not connected yet.");
        ImGui::SetNextItemWidth(300);
        dirty_ |= ImGui::SliderInt("Master volume", &draft_.master_volume, 0, 100, "%d%%");
        ImGui::SetNextItemWidth(300);
        dirty_ |= ImGui::SliderInt("Music", &draft_.music_volume, 0, 100, "%d%%");
        ImGui::SetNextItemWidth(300);
        dirty_ |= ImGui::SliderInt("Sound effects", &draft_.effects_volume, 0, 100, "%d%%");
        dirty_ |= ImGui::Checkbox("Mute all audio", &draft_.mute);
    }

    void ControlsPage()
    {
        PageTitle("Controls", "Save input preferences and check connected controller input.");
        dirty_ |= Choice("Preferred input", draft_.input, {{"auto", "Automatic"}, {"keyboard", "Keyboard"}, {"controller", "Controller"}});
        ImGui::SetNextItemWidth(260);
        dirty_ |= ImGui::SliderInt("Stick deadzone", &draft_.deadzone, 0, 50, "%d%%");
        dirty_ |= ImGui::Checkbox("Controller vibration", &draft_.rumble);
        Paragraph("Preferences are saved. Wii control mapping and gameplay input still need integration.");
        ImGui::Separator();
        ImGui::TextUnformatted("Live controller check");
        if (!gamepad_) Paragraph("No controller detected. Connect a controller to test its sticks and buttons.");
        else
        {
            const char* name = SDL_GetGamepadName(gamepad_);
            ImGui::TextColored(good, "%s", name ? name : "Connected controller");
            const float x = float(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX)) / 32768.0f;
            const float y = float(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY)) / 32768.0f;
            ImGui::Text("Left stick: X %+.2f   Y %+.2f", x, y);
            const SDL_GamepadButton buttons[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
            const char* labels[] = {"South", "East", "West", "North"};
            for (int i = 0; i < 4; ++i)
            {
                if (i) ImGui::SameLine();
                ImGui::TextColored(SDL_GetGamepadButton(gamepad_, buttons[i]) ? good : muted, "%s", labels[i]);
            }
        }
    }

    void AboutPage()
    {
        PageTitle("About this build", "Mario Strikers Charged - Native Port");
        ImGui::Text("Version: %s", mscharged::build::version);
        ImGui::Text("Configuration: %s", MSCHARGED_BUILD_CONFIG);
        ImGui::Spacing();
        Paragraph("This build includes the launcher, settings, controller detection, and ISO/RVZ disc checks. The game is not playable yet.");
#ifdef MSCHARGED_HAS_GAME_STARTUP
        Paragraph("Try startup enters the available original initialization code and reports its first missing service. This prototype cannot run a match yet.");
#else
        Paragraph("The decompilation is still incomplete. Game startup, Aurora rendering, audio, and Wii input adaptation are the next integration steps.");
#endif
        ImGui::Separator();
        Paragraph("Original port code: CC0 1.0. Dependencies retain their own licenses.");
        Paragraph("Header artwork: SteamGridDB hero 8926, uploaded by Jiquita. Font: Roboto Medium (Apache 2.0).");
        Paragraph("Unofficial project, unaffiliated with Nintendo or Next Level Games.");
    }

    void Draw(bool& quit)
    {
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        const float hero_height = HeroHeight();
        ImGui::Dummy({size.x, hero_height});
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilledMultiColor({0, 0}, {size.x * 0.75f, hero_height},
            IM_COL32(9, 14, 23, 210), IM_COL32(9, 14, 23, 0), IM_COL32(9, 14, 23, 20), IM_COL32(9, 14, 23, 230));
        draw->AddText(normal_, 17, {24, 18}, IM_COL32(169, 201, 232, 255), "NATIVE PORT  /  DEVELOPMENT BUILD");
        draw->AddText(heading_, 24, {24, hero_height * 0.36f}, IM_COL32(241, 246, 255, 255), "MARIO STRIKERS");
        draw->AddText(title_, 36, {23, hero_height * 0.36f + 28}, IM_COL32(255, 255, 255, 255), "CHARGED");

        ImGui::SetCursorPos({12, hero_height + 12});
        const float body_height = std::max(220.0f, size.y - hero_height - 94);
        ImGui::BeginChild("navigation", {132, body_height}, ImGuiChildFlags_Borders);
        for (int i = 0; i < 5; ++i)
            if (ImGui::Selectable(pages[i], page_ == i, 0, {0, 28})) page_ = i;
        ImGui::EndChild();
        ImGui::SameLine(156);
        ImGui::BeginChild("page", {size.x - 168, body_height}, ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(!config_ok_);
        switch (page_)
        {
        case 0: GamePage(); break;
        case 1: DisplayPage(); break;
        case 2: AudioPage(); break;
        case 3: ControlsPage(); break;
        default: AboutPage(); break;
        }
        ImGui::EndDisabled();
        ImGui::EndChild();

        ImGui::SetCursorPos({16, size.y - 70});
        ImGui::TextColored(notice_error_ ? bad : muted, "%s", notice_.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", notice_.c_str(), PathUtf8(file_.path).c_str());
        if (dirty_)
        {
            ImGui::SameLine();
            ImGui::TextColored(warning, "Unsaved changes");
        }
        ImGui::SetCursorPos({16, size.y - 44});
#ifdef MSCHARGED_HAS_GAME_STARTUP
        const bool startup_ready = config_ok_ && !pending_ && check_.info
            && check_.selection == draft_.disc && check_.info->game_id == "R4QE01" && check_.info->revision == 1;
        ImGui::BeginDisabled(!startup_ready);
        if (ImGui::Button("Try startup", {104, 32}) && ((!dirty_ && file_.exists) || Save()))
        { startup_config_ = file_.path; quit = true; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Experimental startup for USA revision 1. Saves settings, closes the launcher, and reports the first missing game service in the terminal and startup log.");
#else
        ImGui::BeginDisabled();
        ImGui::Button("Play game", {104, 32});
        ImGui::EndDisabled();
#endif
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(warning, "Game not playable yet");
        ImGui::SameLine(size.x - 324);
        ImGui::BeginDisabled(!config_ok_);
        if (ImGui::Button("Defaults", {82, 32}))
        { auto disc = draft_.disc; draft_ = {}; draft_.disc = disc; dirty_ = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reload", {82, 32})) Reload();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.14f, 0.43f, 0.72f, 1});
        ImGui::BeginDisabled(!config_ok_ || (!dirty_ && file_.exists));
        if (ImGui::Button("Save settings", {128, 32})) Save();
        ImGui::EndDisabled();
        ImGui::PopStyleColor();

        if (close_requested_)
        {
            if (!dirty_) quit = true;
            else ImGui::OpenPopup("Unsaved settings");
            close_requested_ = false;
        }
        if (ImGui::BeginPopupModal("Unsaved settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Save your changes before closing?");
            if (ImGui::Button("Save and close")) { if (Save()) quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Discard")) { quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    Options options_;
    fs::path startup_config_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* header_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;
    ImFont* normal_ = nullptr;
    ImFont* heading_ = nullptr;
    ImFont* title_ = nullptr;
    float header_ratio_ = 620.0f / 1920.0f;
    float header_width_ = 1920;
    float header_height_ = 620;
    bool window_ui_ready_ = false;
    bool renderer_ui_ready_ = false;
    ConfigFile file_;
    Settings draft_;
    bool config_ok_ = false;
    bool dirty_ = false;
    bool notice_error_ = false;
    std::string notice_;
    int page_ = 0;
    bool close_requested_ = false;
    bool pending_ = false;
    DiscCheck check_;
    std::future<DiscCheck> future_;
    std::shared_ptr<DialogResult> dialog_ = std::make_shared<DialogResult>();
};
}

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--version") { std::cout << "mscharged " << mscharged::build::version << " (" << MSCHARGED_BUILD_CONFIG << "; launcher)\n"; return 0; }
        if (arg == "--help")
        {
            std::cout << "Usage: mscharged [--config FILE] [--version]\n"
                         "Development checks: --smoke-test | --screenshot FILE [--page game|display|audio|controls|about]\n";
#ifdef MSCHARGED_HAS_GAME_STARTUP
            std::cout << "Original startup prototype: --experimental-startup [--config FILE] (not playable)\n";
#endif
#ifdef MSCHARGED_HAS_SCENE_PREVIEW
            std::cout << "Static Wii asset preview: --experimental-scene [--config FILE] [--frames N]\n"
                         "                        [--model /DISC/PATH.rlg] [--textures /DISC/PATH.rlt] [--model-id HEX]\n"
                         "                        [--unlit] [--shadow-textures /DISC/PATH.rlt --shadow-id HEX]\n";
#endif
            return 0;
        }
        if (arg == "--smoke-test") options.smoke_test = true;
        else if (arg == "--experimental-startup") options.experimental_startup = true;
        else if (arg == "--experimental-scene") options.experimental_scene = true;
        else if (arg == "--unlit") { options.scene_arguments = true; options.scene.unlit = true; }
        else if ((arg == "--frames" || arg == "--model" || arg == "--textures" || arg == "--model-id"
                  || arg == "--shadow-textures" || arg == "--shadow-id") && i + 1 < argc)
        {
            options.scene_arguments = true;
            const std::string value = argv[++i];
            if (arg == "--model") options.scene.model = value;
            else if (arg == "--textures") options.scene.textures = value;
            else if (arg == "--shadow-textures") options.scene.shadow_textures = value;
            else
            {
                std::uint32_t number = 0;
                std::string_view digits = value;
                const bool hex = arg == "--model-id" || arg == "--shadow-id";
                if (hex && (digits.size() >= 2 && digits.substr(0, 2) == "0x")) digits.remove_prefix(2);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number, hex ? 16 : 10);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()
                    || (arg == "--frames" && (!number || number > 10000)))
                { std::cerr << "Invalid " << arg << " value: " << value << '\n'; return 2; }
                if (arg == "--frames") options.scene.frames = number;
                else if (arg == "--shadow-id") options.scene.shadow_id = number;
                else options.scene.model_id = number;
            }
        }
        else if ((arg == "--config" || arg == "--screenshot" || arg == "--page") && i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (arg == "--config") options.config = PathFromUtf8(value);
            else if (arg == "--screenshot") options.screenshot = PathFromUtf8(value);
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
    if ((options.experimental_startup || options.experimental_scene)
        && (options.smoke_test || !options.screenshot.empty() || options.page_selected
            || (options.experimental_startup && options.experimental_scene)))
    { std::cerr << "Select one runtime mode; capture/smoke options require the launcher.\n"; return 2; }
    if (options.scene_arguments && !options.experimental_scene)
    { std::cerr << "Asset/frame options require --experimental-scene.\n"; return 2; }
    if (options.scene.shadow_id.has_value() != options.scene.shadow_textures.has_value())
    { std::cerr << "--shadow-textures and --shadow-id must be supplied together.\n"; return 2; }
    try
    {
        if (options.experimental_scene)
        {
#ifdef MSCHARGED_HAS_SCENE_PREVIEW
            if (options.config.empty())
            {
                const char* base = SDL_GetBasePath();
                Require(base != nullptr, "Cannot locate the executable");
                options.config = DefaultConfig(PathFromUtf8(base));
            }
            return RunScenePreview(argc, argv, options.config, options.scene);
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
            return RunGameStartup(argc, argv, options.config);
#else
            std::cerr << "Experimental startup is not in this build. Use cmake --workflow --preset startup.\n";
            return 2;
#endif
        }
        fs::path startup_config;
        int result;
        {
            Launcher launcher;
            result = launcher.Run(std::move(options));
            startup_config = launcher.StartupConfig();
        } // Destroy the launcher/ImGui/SDL session before Aurora initializes.
#ifdef MSCHARGED_HAS_GAME_STARTUP
        if (!startup_config.empty()) return RunGameStartup(argc, argv, startup_config);
#endif
        return result;
    }
    catch (const std::exception& e) { std::cerr << "Launcher failed: " << e.what() << '\n'; return 1; }
}
