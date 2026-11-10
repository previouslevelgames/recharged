#include "runtime/host_menus.h"

#include "bootstrap/texts.h"
#include "platform/session_log.h"

#include <aurora/imgui.h>
#include <aurora/video.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace mscharged::runtime {
namespace {
// The game's MENU IN / MENU OUT timing: the screen drops in (rises away) in
// 0.1 s; what sits on it appears 0.15 s after the menu opens. A page's
// content comes in over 0.15 s.
constexpr float kDropSeconds = 0.1f, kContentDelay = 0.15f, kContentFade = 0.05f, kPageFade = 0.15f;
// The game's header bar where its pause menu has it (front end units:
// centre, size).
constexpr float kHeaderX = 0.0f, kHeaderY = 174.0f, kHeaderW = 400.0f, kHeaderH = 55.0f;
// The pause menu's buttons where the game's layout has them (before its
// pointer regions are set), in design pixels.
constexpr float kFirstButtonY = -300.0f, kButtonStep = 99.6f, kButtonHalfWidth = 402.0f;
// A slider's change, applied at once while it moves, is saved once it has
// rested this long.
constexpr auto kSaveRest = std::chrono::milliseconds(400);

// A pause menu button's text when the game has none (the game's English
// ones), by its ePauseMenuAction.
const char* FallbackLabel(int action)
{
    switch (action) {
    case 0: return texts::T("menus.pause.resume");
    case 1: return texts::T("menus.pause.choose_sides");
    case 2: return texts::T("menus.pause.audio_options");
    case 3: return texts::T("menus.pause.visual_options");
    case 4: return texts::T("menus.pause.statistics");
    case 5: return texts::T("menus.pause.quit");
    case 6: return texts::T("menus.pause.controller_map");
    default: return "";
    }
}

float EaseOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
float EaseIn(float t) { return t * t * t; }
float Seconds(HostMenus::Clock::duration d) { return std::chrono::duration<float>(d).count(); }

// One of the keys pressed this frame (repeating while held, or not).
bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat = true)
{
    for (const ImGuiKey key : keys)
        if (ImGui::IsKeyPressed(key, repeat)) return true;
    return false;
}

// One of them held long enough that it repeats.
bool Repeating(std::initializer_list<ImGuiKey> keys)
{
    for (const ImGuiKey key : keys) {
        const ImGuiKeyData* data = ImGui::GetKeyData(key);
        if (data && data->Down && data->DownDuration > ImGui::GetIO().KeyRepeatDelay) return true;
    }
    return false;
}

// The pages that show Online or Ranked (a player's page's title too).
bool OnlinePage(ui::PageId id)
{
    switch (id) {
    case ui::PageId::Online:
    case ui::PageId::Players:
    case ui::PageId::Player:
    case ui::PageId::Setup:
    case ui::PageId::Ranked:
    case ui::PageId::Leaderboard: return true;
    default: return false;
    }
}
} // namespace

HostMenus::HostMenus(Game game, MenuInput& input, Host host) : game_(game), input_(input), host_(std::move(host))
{
    input_.SetMenuKeys([this](MenuInput::MenuKey key, int channel) { MenuKey(key, channel); });
}

HostMenus::~HostMenus()
{
    input_.SetMenuKeys(nullptr);
    input_.ClosePages();
    if (blurred_) aurora_set_video_output_blur(false);
}

void HostMenus::Command(int kind, int which, int value)
{
    // (In a match against another machine the module takes only the cursors'
    // drawing and the forfeit: charged_menu_command.)
    if (!game_.command) return;
    ChargedMenuCommand command{kind, which, value};
    game_.command(&command, sizeof command);
}

void HostMenus::Play(ui::Sound sound)
{
    int kind = CHARGED_SOUND_HOVER;
    switch (sound) {
    case ui::Sound::Hover: kind = CHARGED_SOUND_HOVER; break;
    case ui::Sound::Select: kind = CHARGED_SOUND_SELECT; break;
    case ui::Sound::Back: kind = CHARGED_SOUND_BACK; break;
    case ui::Sound::Step: kind = CHARGED_SOUND_STEP; break;
    case ui::Sound::Switch: kind = CHARGED_SOUND_SWITCH; break;
    }
    Command(CHARGED_MENU_SOUND, kind);
}

void HostMenus::Push(ui::PageId id, int category, std::string key, Origin origin)
{
    pages_.push_back({id, category, std::move(key), origin, {}});
    pageAt_ = Clock::now();
}

void HostMenus::Pop()
{
    if (pages_.empty()) return;
    if (gameChanged_ && pages_.back().id == ui::PageId::Settings) {
        Command(CHARGED_MENU_SAVE);
        gameChanged_ = false;
    }
    pages_.pop_back();
    pageAt_ = Clock::now();
}

void HostMenus::ClearPages()
{
    if (gameChanged_) Command(CHARGED_MENU_SAVE);
    gameChanged_ = false;
    pages_.clear();
    pageAt_ = Clock::now();
}

bool HostMenus::Opened(Origin origin) const
{
    return std::any_of(pages_.begin(), pages_.end(), [origin](const Page& page) { return page.origin == origin; });
}

void HostMenus::CloseAll()
{
    if (pages_.empty()) return;
    // The front end's Options hub comes again only with the scene's next visit.
    if (Opened(Origin::Anchored)) anchorDone_ = true;
    ClearPages();
}

void HostMenus::LeaveMatch() { Command(CHARGED_MENU_FORFEIT); }

void HostMenus::CloseRecharged()
{
    while (RechargedOpen()) Pop();
}

bool HostMenus::Leaving() const
{
    return (menu_.forfeiting || forfeitAsked_) && !pages_.empty() && pages_.back().id == ui::PageId::Forfeit;
}

void HostMenus::SetPrompt(std::optional<overlay::OverlayPrompt> prompt)
{
    const bool had = prompt_.has_value();
    prompt_ = std::move(prompt);
    if (prompt_ && !had) Play(ui::Sound::Select);
}

std::optional<int> HostMenus::TakePromptAnswer() { return std::exchange(promptAnswer_, std::nullopt); }

void HostMenus::MenuKey(MenuInput::MenuKey key, int channel)
{
    // The game's pause in an online match, which the game refuses: the match
    // menu (nothing pauses). Elsewhere the press is only the game's.
    if (key == MenuInput::MenuKey::Pause) {
        if (menu_.network_match != CHARGED_NETWORK_ONLINE || !pages_.empty()) return;
        channel_ = channel;
        Push(ui::PageId::Match, 0, {}, Origin::Recharged);
        return;
    }
    // A question waits for its answer first, and a forfeited match's way out
    // for the game.
    if (!pages_.empty() && pages_.back().id == ui::PageId::Prompt) return;
    if (Leaving()) return;
    if (RechargedOpen()) {
        // Closed: its pages go, back to what was open under it.
        Play(ui::Sound::Back);
        CloseRecharged();
        return;
    }
    channel_ = channel;
    Push(ui::PageId::Menu, 0, {}, Origin::Recharged);
    // F2: straight to Recharged's gameplay options.
    if (key == MenuInput::MenuKey::Options) {
        Push(ui::PageId::Options);
        Push(ui::PageId::Gameplay);
    }
    // Over a match in play the game pauses (not a network match: it can't).
    pausePress_ = !menu_.network_match;
    Play(ui::Sound::Select);
}

void HostMenus::Back()
{
    if (pages_.empty() || Leaving()) return;
    Play(ui::Sound::Back);
    // The prompt: its second answer ("Decline", "Not now").
    if (pages_.back().id == ui::PageId::Prompt) {
        promptAnswer_ = 2;
        return;
    }
    // The front end's Options hub: back to the main menu, the game's own way.
    if (pages_.size() == 1 && pages_.front().origin == Origin::Anchored) {
        Command(CHARGED_MENU_BACK);
        anchorDone_ = true;
        ClearPages();
        return;
    }
    Pop();
}

void HostMenus::Resume()
{
    ClearPages();
    Command(CHARGED_MENU_RESUME);
}

void HostMenus::Apply(const ui::PageActions& actions, bool live, const overlay::GameOverlayModel& model)
{
    if (actions.setting && host_.setting) {
        auto change = *actions.setting;
        change.live = live;
        host_.setting(change);
        if (live) {
            change.live = false;
            pendingSave_ = change;
            lastLive_ = Clock::now();
        } else pendingSave_.reset();
    }
    if (actions.option && host_.option) host_.option(*actions.option);
    if (actions.music) Command(CHARGED_MENU_VOLUME, 0, *actions.music);
    if (actions.effects) Command(CHARGED_MENU_VOLUME, 1, *actions.effects);
    if (actions.voices) {
        Command(CHARGED_MENU_VOLUME, 2, *actions.voices);
        Command(CHARGED_MENU_VOICE);
    }
    if (actions.auto_zoom) Command(CHARGED_MENU_AUTO_ZOOM, 0, *actions.auto_zoom ? 1 : 0);
    if (actions.zoom) Command(CHARGED_MENU_ZOOM, 0, *actions.zoom);
    if (actions.music || actions.effects || actions.voices || actions.auto_zoom || actions.zoom) gameChanged_ = true;
    if (actions.open) Push(*actions.open, 0, actions.open_key);
    if (actions.answer) promptAnswer_ = actions.answer;
    if (actions.forfeit && !forfeitAsked_) {
        platform::SessionLogLine("Menus: FORFEIT (the online match is left)");
        forfeitAsked_ = true;
        if (host_.forfeit) host_.forfeit();
        else LeaveMatch();
    }
    if (actions.wii_online) {
        // The game's own online, as its main menu's globe: these pages go.
        Command(CHARGED_MENU_WII_ONLINE);
        ClearPages();
        return;
    }
    if ((actions.online.Any() || actions.ranked.Any()) && host_.online
        && host_.online(actions.online, actions.ranked, model))
        ClearPages();
    if (actions.credits) {
        Command(CHARGED_MENU_PRESS, 2);
        anchorDone_ = true;
        ClearPages();
    }
    if (actions.close) CloseRecharged();
    if (actions.back) Back();
    if (actions.quit && host_.quit) {
        platform::SessionLogLine("Menus: QUIT RECHARGED");
        host_.quit();
    }
}

void HostMenus::ShowPrompt(Clock::time_point now)
{
    const bool onTop = !pages_.empty() && pages_.back().id == ui::PageId::Prompt;
    if (prompt_ && onTop) return;
    const auto before = pages_.size();
    pages_.erase(std::remove_if(pages_.begin(), pages_.end(),
                                [](const Page& page) { return page.id == ui::PageId::Prompt; }),
                 pages_.end());
    if (prompt_) Push(ui::PageId::Prompt);
    else if (pages_.size() != before) pageAt_ = now;
}

void HostMenus::Service(const ChargedMenuSnapshot* menu, Clock::time_point now)
{
    known_ = menu != nullptr;
    menu_ = known_ ? *menu : ChargedMenuSnapshot{};
    if (menu_.fe_height > 0.0f) feHeight_ = menu_.fe_height;
    const int scene = menu_.menu;

    // Each player's hand as the game drew it, around its pointer (the A
    // included), for the hand at the mouse: wherever the game draws it.
    for (auto& hand : newHands_) hand.clear();
    for (int n = 0; n < std::min(menu_.quad_count, int(std::size(menu_.quads))); ++n) {
        ChargedMenuSnapshot::Quad quad = menu_.quads[n];
        if (quad.pad < 0 || quad.pad > 3) continue;
        for (int k = 0; k < 4; ++k) {
            quad.x[k] -= menu_.pointer_x[quad.pad];
            quad.y[k] -= menu_.pointer_y[quad.pad];
        }
        newHands_[std::size_t(quad.pad)].push_back(quad);
    }
    for (std::size_t pad = 0; pad < 4; ++pad)
        if (!newHands_[pad].empty()) std::swap(hands_[pad], newHands_[pad]);

    // The game's buttons the module took: the pause menu's Audio and Visual
    // Options (SETTINGS on SOUND or DISPLAY over it), the main menu's globe
    // (the ONLINE hub over the wheel).
    if (known_) {
        if (taken_ && menu_.taken != *taken_ && pages_.empty()) {
            if (menu_.taken_menu == CHARGED_MENU_PAUSE && scene == CHARGED_MENU_PAUSE) {
                Push(ui::PageId::Settings, menu_.taken_button == 2 ? ui::kSettingsSound : ui::kSettingsDisplay, {},
                     Origin::Pause);
                channel_ = menu_.taken_pad;
            } else if (menu_.taken_menu == CHARGED_MENU_MAIN) {
                Push(ui::PageId::Online, 0, {}, Origin::Wheel);
                channel_ = menu_.taken_pad;
            }
        }
        taken_ = menu_.taken;
    }
    // The pause menu gone (resumed, quit) under its pages: they go too.
    if (Opened(Origin::Pause) && scene != CHARGED_MENU_PAUSE) ClearPages();
    // The match menu without its match (over, left, forfeited): it goes.
    if (!menu_.network_match) forfeitAsked_ = false;
    if (!menu_.network_match && std::any_of(pages_.begin(), pages_.end(), [](const Page& page) {
            return page.id == ui::PageId::Match || page.id == ui::PageId::Forfeit;
        }))
        ClearPages();
    // The front end's Options: the hub once its slide is in, until it goes.
    if (scene == CHARGED_MENU_OPTIONS) {
        if (menu_.ready && !anchorDone_ && pages_.empty()) {
            channel_ = -1;
            Push(ui::PageId::Options, 0, {}, Origin::Anchored);
        }
        if (menu_.leaving && Opened(Origin::Anchored)) {
            anchorDone_ = true;
            ClearPages();
        }
    } else {
        anchorDone_ = false;
        if (Opened(Origin::Anchored)) ClearPages();
    }
    ShowPrompt(now);

    // The input: the menus' while they show a page.
    const bool wantInput = !pages_.empty();
    if (wantInput && (!input_.PagesOpen() || pausePress_)) {
        if (input_.PagesOpen()) input_.ClosePages();
        input_.OpenPages(channel_, pausePress_);
        pointerShown_ = false;
        lastPointer_.reset();
    } else if (!wantInput && input_.PagesOpen()) input_.ClosePages();
    if (wantInput || pages_.empty()) pausePress_ = false;

    // The TV: over the pause menu (not while it goes), with a page, or for
    // the front end's Options until they go.
    const bool pauseShown = scene == CHARGED_MENU_PAUSE && !menu_.leaving;
    const bool optionsShown = scene == CHARGED_MENU_OPTIONS && !menu_.leaving && !anchorDone_;
    const bool wantTv = !pages_.empty() || pauseShown || optionsShown;
    if (wantTv) {
        if (!shown_ || leaving_) {
            shownAt_ = now;
            // It opens at its page's width.
            halfWidth_ = !pages_.empty() && ui::PageWide(pages_.back().id) ? ui::kWideHalfWidth
                                                                          : ui::kStandardHalfWidth;
        }
        shown_ = true;
        leaving_ = false;
    } else if (shown_ && !leaving_) {
        leaving_ = true;
        leavingAt_ = now;
    }
    if (leaving_ && now - leavingAt_ > std::chrono::milliseconds(250)) shown_ = leaving_ = false;
    // (Not over a network match: it plays on behind.)
    const bool wantBlur = shown_ && !leaving_ && !menu_.network_match
        && (scene == CHARGED_MENU_PAUSE || RechargedOpen());
    if (wantBlur != blurred_) {
        aurora_set_video_output_blur(wantBlur);
        blurred_ = wantBlur;
    }

    // The game hands its players' cursors over while a page of these shows
    // (the hand at the mouse is the player's own).
    const bool wantCursors = shown_ && !pages_.empty();
    if (wantCursors != cursors_) {
        Command(CHARGED_MENU_CURSORS, 0, wantCursors ? 1 : 0);
        cursors_ = wantCursors;
    }

    // A slider's last change saved once it rests.
    if (pendingSave_ && now - lastLive_ >= kSaveRest) {
        if (host_.setting) host_.setting(*pendingSave_);
        pendingSave_.reset();
    }
}

void* HostMenus::Texture(unsigned key)
{
    if (const auto known = textures_.find(key); known != textures_.end()) return known->second;
    void* made = nullptr;
    int width = 0, height = 0, format = -1;
    if (game_.texture && game_.texture(key, &width, &height, &format, nullptr, 0) && width > 0 && height > 0
        && width <= 2048 && height <= 2048) {
        std::vector<unsigned char> rgba(std::size_t(width) * std::size_t(height) * 4);
        if (game_.texture(key, &width, &height, &format, rgba.data(), unsigned(rgba.size())))
            made = reinterpret_cast<void*>(aurora_imgui_add_texture(std::uint32_t(width), std::uint32_t(height),
                                                                    rgba.data()));
    }
    // Kept, also when it failed: the overlay cannot free a texture, and a key
    // the game cannot give is not asked for again.
    textures_.emplace(key, made);
    char line[96];
    std::snprintf(line, sizeof line, "Menus: game texture %08x %s (%dx%d, GX format %d)", key,
                  made ? "made" : "unavailable", width, height, format);
    platform::SessionLogLine(line);
    return made;
}

void* HostMenus::NoiseTexture()
{
    if (noiseMade_) return noise_;
    noiseMade_ = true;
    const auto rgba = ui::NoiseTexels();
    noise_ = reinterpret_cast<void*>(aurora_imgui_add_texture(ui::kNoiseSize, ui::kNoiseSize, rgba.data()));
    return noise_;
}

HostMenus::Frame HostMenus::Locate(float width, float height, Clock::time_point now) const
{
    Frame frame{};
    frame.elapsed = Seconds(now - shownAt_);
    // The game's MENU IN: the picture darkens and the screen drops in from
    // above in 0.1 s; the header, title and content follow at 0.15 s. MENU
    // OUT: those go at once, the picture brightens in 0.1 s and the screen
    // rises away in the next 0.1 s.
    frame.dim = std::clamp(frame.elapsed / kDropSeconds, 0.0f, 1.0f);
    frame.drop = 1.0f - EaseOut(std::clamp(frame.elapsed / kDropSeconds, 0.0f, 1.0f));
    frame.content = std::clamp((frame.elapsed - kContentDelay) / kContentFade, 0.0f, 1.0f);
    if (leaving_) {
        const float gone = Seconds(now - leavingAt_);
        frame.dim *= 1.0f - std::clamp(gone / kDropSeconds, 0.0f, 1.0f);
        frame.drop = std::max(frame.drop, EaseIn(std::clamp((gone - kDropSeconds) / kDropSeconds, 0.0f, 1.0f)));
        frame.content = 0.0f;
    }
    frame.pageIn = std::clamp(Seconds(now - pageAt_) / kPageFade, 0.0f, 1.0f);
    // The game's picture in the window (its shape kept, or stretched), and
    // the front end's units in it: the game's pointers and buttons use them.
    float vx = 0, vy = 0, vw = width, vh = height;
    AuroraVIPresentedGeometry geometry{};
    if (aurora_get_presented_video_geometry(&geometry) && geometry.viewport_width > 0 && geometry.viewport_height > 0
        && geometry.surface_width == std::uint32_t(width) && geometry.surface_height == std::uint32_t(height)) {
        vx = geometry.viewport_x, vy = geometry.viewport_y, vw = geometry.viewport_width, vh = geometry.viewport_height;
    }
    frame.unit = vh / feHeight_;
    frame.middle = {vx + vw * 0.5f, vy + vh * 0.5f};
    frame.width = vw;
    return frame;
}

HostMenus::Tv HostMenus::LayOut(const Frame& frame, Clock::time_point now)
{
    Tv tv{};
    const float s = frame.unit / ui::kDesignPerUnit;
    const bool pages = !pages_.empty();
    // The frame: wide for a page with categories (as much as the picture
    // allows), the pause menu's otherwise, moving between them smoothly.
    const bool wide = pages && ui::PageWide(pages_.back().id);
    const float room = std::max(ui::kStandardHalfWidth, frame.width * 0.5f / s - 40.0f);
    ui::Ease(halfWidth_, wide ? std::min(ui::kWideHalfWidth, room) : ui::kStandardHalfWidth);
    tv.half = halfWidth_;
    // A question or the match menu (ui::PageCompact) gets a TV of its own
    // size: its screen as tall as its items, no lip under it (B answers it),
    // centred where the full TV is. A TV that opens does so at its size.
    tv.compact = pages && ui::PageCompact(pages_.back().id);
    tv.alone = tv.compact && pages_.size() == 1 && menu_.menu != CHARGED_MENU_PAUSE;
    tv.contentTop = tv.alone ? ui::kHeaderBottom + 16.0f : ui::kContentTop;
    const float wantScreen = tv.compact
        ? std::min(ui::kScreenBottom, tv.contentTop + std::max(pages_.back().list.height, 160.0f) + 26.0f)
        : ui::kScreenBottom;
    const float wantFrame = tv.compact ? wantScreen + ui::kBezel : ui::kFrameBottom;
    if (Seconds(now - drawnAt_) > 0.25f) screenBottom_ = wantScreen, frameBottom_ = wantFrame;
    drawnAt_ = now;
    ui::Ease(screenBottom_, wantScreen);
    ui::Ease(frameBottom_, wantFrame);
    tv.screenBottom = screenBottom_;
    // Centred: lowered by half of what the frame is shorter; dropping in.
    const float lower = (ui::kFrameBottom - frameBottom_) * 0.5f - frame.drop * ui::kDesignHeight;
    tv.set = {ImGui::GetForegroundDrawList(), s, {frame.middle.x, frame.middle.y + lower * s}, 1.0f};
    tv.screen = tv.set.Faded(frame.content);
    tv.body = tv.screen.Faded(frame.pageIn);
    return tv;
}

void HostMenus::DrawHeader(const Frame& frame, const Tv& tv)
{
    // The game's own picture (the orange bar with its logo) where the game
    // puts it (as its pause menu, when the module only found it by name);
    // else one drawn alike.
    void* header = menu_.header_texture ? Texture(menu_.header_texture) : nullptr;
    if (header) header_ = header;
    const bool own = header && menu_.header_width > 0.0f;
    const float hx = own ? menu_.header_x : kHeaderX, hy = own ? menu_.header_y : kHeaderY;
    const float hw = (own ? menu_.header_width : kHeaderW) * 0.5f;
    const float hh = (own ? menu_.header_height : kHeaderH) * 0.5f;
    const float lower = tv.set.c.y - frame.middle.y; // the TV's own shift
    const auto at = [&](float x, float y) {
        const ImVec2 point = frame.FromUnits(x, y);
        return ImVec2{point.x, point.y + lower};
    };
    const ImVec2 corners[4] = {at(hx - hw, hy + hh), at(hx + hw, hy + hh), at(hx + hw, hy - hh),
                               at(hx - hw, hy - hh)};
    ui::DrawHeader(tv.screen, tv.half, ImTextureID(header_), corners);
}

HostMenus::ModelNeeds HostMenus::Needs() const
{
    ModelNeeds needs;
    needs.online = std::any_of(pages_.begin(), pages_.end(), [](const Page& page) { return OnlinePage(page.id); });
    needs.leaderboard = !pages_.empty() && pages_.back().id == ui::PageId::Leaderboard;
    needs.settings = !pages_.empty() && pages_.back().id == ui::PageId::Settings;
    return needs;
}

void HostMenus::BuildModel(ui::PageModel& model)
{
    if (host_.model) host_.model(model, Needs());
    model.image = host_.image ? &host_.image : nullptr;
    model.prompt = prompt_ ? &*prompt_ : nullptr;
    model.from_wheel = Opened(Origin::Wheel);
    model.network_match = menu_.network_match != 0;
    model.forfeiting = menu_.forfeiting != 0 || forfeitAsked_;
    model.front_end = known_ ? menu_.front_end != 0 : true;
    model.game.known = known_;
    model.game.music = menu_.music_volume;
    model.game.effects = menu_.effects_volume;
    model.game.voices = menu_.voice_volume;
    model.game.auto_zoom = menu_.auto_zoom != 0;
    model.game.zoom = menu_.zoom_level;
}

ui::Input HostMenus::ReadInput()
{
    ui::Input in;
    ImGuiIO& io = ImGui::GetIO();
    const bool valid = ImGui::IsMousePosValid();
    in.pointer = io.MousePos;
    // (Not on the first frame the pages have the input.)
    in.pointer_moved = valid && lastPointer_ && (io.MousePos.x != lastPointer_->x || io.MousePos.y != lastPointer_->y);
    lastPointer_ = io.MousePos;
    if (in.pointer_moved) pointerShown_ = true;
    in.pointer_shown = pointerShown_ && valid;
    in.click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    in.held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    in.wheel = io.MouseWheel;
    in.up = Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp});
    in.down = Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown});
    in.left = Pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft});
    in.right = Pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight});
    in.accept = Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown}, false);
    in.back = Pressed({ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight}, false)
        || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    in.resume = Pressed({ImGuiKey_GamepadFaceLeft}, false);
    // The categories beside: - and + (the keyboard's - and Tab), Q and E.
    in.previous = Pressed({ImGuiKey_GamepadL1, ImGuiKey_Minus, ImGuiKey_Q, ImGuiKey_PageUp}, false);
    in.next = Pressed({ImGuiKey_GamepadR1, ImGuiKey_Tab, ImGuiKey_E, ImGuiKey_PageDown}, false);
    in.repeat = Repeating({ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                           ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight});
    return in;
}

bool HostMenus::DrawTopPage(const Tv& tv, const ui::Fonts& fonts, const ui::Input& in, const ui::PageModel& model,
                            const std::string& title, Clock::time_point now)
{
    const auto sound = [this](ui::Sound s) { Play(s); };
    Page& page = pages_.back();
    const int scene = menu_.menu;
    const bool pauseUnder = scene == CHARGED_MENU_PAUSE;
    const auto& categories = ui::PageCategories(page.id);
    const float iw = tv.half - ui::kBezel;
    const float top = tv.contentTop, bottom = tv.screenBottom - 14.0f;
    ui::Area rows{-iw + 24.0f, iw - 24.0f, top, bottom};
    // Where the page is, in the strip under the header: PAUSED > SETTINGS
    // > SOUND (a question alone has none: its title says it). A step before
    // the last takes the player back to it.
    int crumb = -1;
    if (!tv.alone) {
        crumbs_.clear();
        if (pauseUnder) crumbs_.push_back(menu_.title[0] ? menu_.title : texts::T("menus.pause.title"));
        for (std::size_t n = 0; n + 1 < pages_.size(); ++n)
            crumbs_.push_back(ui::PageTitle(pages_[n].id, pages_[n].key, model));
        crumbs_.push_back(title);
        if (page.category < int(categories.size())) crumbs_.push_back(categories[std::size_t(page.category)]);
        crumb = ui::DrawCrumbs(tv.body, fonts, in, tv.half, crumbs_, sound);
    }
    std::optional<ui::Area> help;
    int category = -1;
    if (!categories.empty()) {
        // The categories at the left (a click picks one), the lit item's
        // help under them, the rows beside.
        const ui::Area column{-iw + 24.0f, -iw + 24.0f + 330.0f, top, bottom};
        float used = 0.0f;
        category = ui::DrawCategories(tv.body, fonts, in, column, categories, page.category, sound, &used);
        help = ui::Area{column.x0, column.x1, top + used + 8.0f, bottom};
        rows.x0 = column.x1 + 36.0f;
        rows.x1 = iw - 36.0f;
    }
    ui::PageActions actions;
    bool overItem = false;
    {
        // The page on top, its items between the strip and the screen's foot.
        ui::List list(tv.body, fonts, in, page.list, sound, rows, help);
        ui::DrawPage(page.id, page.category, page.key, model, pageState_, list, actions);
        overItem = list.PointerOverItem();
    }
    ui::DrawScreenMask(tv.set, tv.half, tv.screenBottom);
    // The footer: the game's big BACK at the left, the way on (RESUME over
    // the pause menu, CLOSE for the Recharged menu) at the right. A question
    // has none: its answers are on its screen.
    int under = -1;
    if (!tv.compact) {
        ui::FooterButton buttons[2] = {{texts::T("menus.footer.back"), false}};
        int count = 1;
        if (pauseUnder) buttons[count++] = {texts::T("menus.footer.resume"), true};
        else if (RechargedOpen()) buttons[count++] = {texts::T("menus.footer.close"), true};
        under = ui::DrawFooter(tv.screen, fonts, tv.half, buttons, count, in.pointer, in.pointer_shown);
    }
    const bool backPressed = in.back || (in.click && under == 0);
    const bool resumePressed = pauseUnder && (in.resume || (in.click && under == 1));
    const bool closePressed = !pauseUnder && in.click && under == 1;
    // Another category: - and +, or a click.
    if (!categories.empty() && (in.previous || in.next) && category < 0) {
        const int count = int(categories.size());
        category = (page.category + (in.next ? 1 : count - 1)) % count;
        Play(ui::Sound::Switch);
    }
    // Then what the player did: the pages may change now. A step of the
    // strip keeps the pages up to it (the pause menu's: none).
    if (leaving_ || Leaving()) return overItem;
    const int keep = crumb < 0 ? -1 : crumb + (pauseUnder ? 0 : 1);
    if (keep >= 0 && keep < int(pages_.size())) {
        if (keep == 0) ClearPages();
        else while (int(pages_.size()) > keep) Pop();
    } else if (resumePressed) Resume();
    else if (closePressed) {
        Play(ui::Sound::Back);
        CloseRecharged();
    } else if (backPressed) Back();
    else if (category >= 0 && category != page.category) {
        page.category = category;
        page.list = {};
        pageAt_ = now;
    } else Apply(actions, in.repeat || in.held, model.overlay);
    return overItem;
}

void HostMenus::DrawPauseButtons(const Frame& frame, const Tv& tv, const ui::Fonts& fonts)
{
    // The game's pause menu: its buttons where its pointers find them (else
    // where its layout has them), lit while a pointer is over one.
    for (int row = 0; row < 7; ++row) {
        const int action = kChargedPauseMenuOrder[row];
        const ChargedMenuSnapshot::Button& button = menu_.buttons[action];
        float mid = kFirstButtonY + kButtonStep * float(row), halfWidth = kButtonHalfWidth;
        if (button.max_x > button.min_x && button.max_y > button.min_y) {
            mid = tv.body
                      .ToDesign(frame.FromUnits((button.min_x + button.max_x) * 0.5f,
                                                (button.min_y + button.max_y) * 0.5f))
                      .y;
            halfWidth = (button.max_x - button.min_x) * 0.5f * ui::kDesignPerUnit;
        }
        const char* label = button.label[0] ? button.label : FallbackLabel(action);
        ui::DrawPlate(tv.body, fonts.plate, halfWidth, mid, label, button.hovered != 0);
    }
}

void HostMenus::DrawHands(const Frame& frame, const ui::Input& in, bool overItem)
{
    // The players' hands on top. On a page: the mouse's, the player's own
    // hand as the game last drew it (its A over an item). Else the game's,
    // as it drew them this frame (its hand, its number, the A over a
    // button), last handed over first (the front end's view draws in
    // reverse order).
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const auto drawQuad = [&](const ChargedMenuSnapshot::Quad& quad, float dx, float dy) {
        void* texture = Texture(quad.texture);
        if (!texture) return;
        ImVec2 corner[4], uv[4];
        for (int k = 0; k < 4; ++k) {
            corner[k] = frame.FromUnits(quad.x[k] + dx, quad.y[k] + dy);
            uv[k] = {quad.u[k], quad.v[k]};
        }
        drawList->AddImageQuad(ImTextureID(texture), corner[0], corner[1], corner[2], corner[3], uv[0], uv[1], uv[2],
                               uv[3], IM_COL32(quad.rgba[0], quad.rgba[1], quad.rgba[2], quad.rgba[3]));
    };
    if (leaving_) return;
    if (pages_.empty()) {
        for (int n = std::min(menu_.quad_count, int(std::size(menu_.quads))) - 1; n >= 0; --n)
            drawQuad(menu_.quads[n], 0.0f, 0.0f);
        return;
    }
    if (!in.pointer_shown) return;
    int pad = input_.KeyboardChannel();
    if (pad < 0 || pad > 3 || hands_[std::size_t(pad)].empty()) pad = menu_.controller;
    if (pad < 0 || pad > 3 || hands_[std::size_t(pad)].empty())
        for (pad = 0; pad < 3 && hands_[std::size_t(pad)].empty(); ++pad) {}
    const auto& hand = hands_[std::size_t(pad)];
    const ImVec2 at = frame.ToUnits(in.pointer);
    for (auto quad = hand.rbegin(); quad != hand.rend(); ++quad)
        if (quad->texture != CHARGED_MENU_CURSOR_A_TEXTURE || overItem) drawQuad(*quad, at.x, at.y);
    // No hand of the game's seen yet (a question before its menus): the
    // plain pointer.
    if (hand.empty())
        ImGui::RenderMouseCursor(in.pointer, std::max(1.0f, frame.unit * 0.5f), ImGuiMouseCursor_Arrow, IM_COL32_WHITE,
                                 IM_COL32_BLACK, IM_COL32(0, 0, 0, 48));
}

void HostMenus::Draw(float width, float height, const ui::Fonts& fonts)
{
    if (!shown_) return;
    const auto now = Clock::now();
    const Frame frame = Locate(width, height, now);
    const int scene = menu_.menu;
    const bool pages = !pages_.empty();
    // The picture behind darkens: over the blurred match as the game's own
    // pause dims it, over the front end's menus a little, over a network
    // match (playing on) less.
    const int shade = menu_.network_match ? 40 : scene == CHARGED_MENU_PAUSE || RechargedOpen() ? 110 : 60;
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {width, height},
                                                  IM_COL32(4, 6, 10, int(float(shade) * frame.dim)));
    const Tv tv = LayOut(frame, now);
    ui::DrawTv(tv.set, tv.screen, {tv.half, frame.elapsed, ImTextureID(NoiseTexture()), tv.screenBottom, frameBottom_});
    DrawHeader(frame, tv);

    // The pages' model, once a frame while they show; the title: the page's,
    // else the game's menu's ("PAUSED").
    ui::PageModel model;
    if (pages) BuildModel(model);
    const std::string title = pages ? ui::PageTitle(pages_.back().id, pages_.back().key, model)
        : scene == CHARGED_MENU_OPTIONS ? std::string(texts::T("menus.options.title"))
                                        : std::string(menu_.title[0] ? menu_.title : texts::T("menus.pause.title"));
    ui::DrawTitle(tv.body, fonts.title, title.c_str(), frame.elapsed);

    // The page with the players' input, or the game's pause menu; the
    // screen's corners over what it shows; the hands on top.
    ui::Input in;
    bool overItem = false;
    if (pages) {
        if (!leaving_) in = ReadInput();
        overItem = DrawTopPage(tv, fonts, in, model, title, now);
    } else {
        if (scene == CHARGED_MENU_PAUSE) DrawPauseButtons(frame, tv, fonts);
        ui::DrawScreenMask(tv.set, tv.half, tv.screenBottom);
    }
    DrawHands(frame, in, overItem);
}

} // namespace mscharged::runtime
