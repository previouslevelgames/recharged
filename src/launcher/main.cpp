#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_stdlib.h"

#include "bootstrap/advanced_choices.h"
#include "bootstrap/antialiasing.h"
#include "bootstrap/display_shape.h"
#include "bootstrap/launch_options.h"
#include "bootstrap/watch_link.h"
#include "platform/watch_link_handler.h"
#include "bootstrap/graphics_backend.h"
#include "bootstrap/table_texts.h"
#include "bootstrap/texts.h"
#include "launcher/background_job.h"
#include "launcher/competitive_service.h"
#include "launcher/current_build.h"
#include "launcher/discord_friends.h"
#include "launcher/disc_search.h"
#include "launcher/image_library.h"
#include "launcher/issue_reporters.h"
#include "launcher/game_pictures.h"
#include "launcher/match_presets.h"
#include "launcher/match_report.h"
#include "launcher/mii_faces.h"
#include "launcher/online_gate.h"
#include "launcher/player_lineup.h"
#include "launcher/players_online.h"
#include "launcher/release_banner.h"
#include "launcher/release_notes_view.h"
#include "launcher/server_message.h"
#include "launcher/texture_pack_library.h"
#include "launcher/fallback_fonts.h"
#include "launcher/ui_kit.h"
#include "launcher/ui_metrics.h"
#include "launcher/update_check.h"
#include "launcher/update_install.h"
#include "launcher/update_service.h"
#include "mscharged/build_version.h"
#include "overlay/flags.h"
#include "platform/competitive_rules.h"
#include "platform/utf8.h"
#include "platform/parse_float.h"
#include "platform/name_glyphs.h"
#include "platform/profile_service.h"
#include "platform/window_fit.h"
#include "platform/disc.h"
#include "platform/game_release.h"
#include "platform/match_replay_file.h"
#include "platform/pc_languages.h"
#include "platform/console_verbosity.h"
#include "platform/log_window.h"
#ifdef __APPLE__
#include "platform/macos_app.h"
#endif
#include "platform/app_icon.h"
#include "platform/path.h"
#include "platform/mii_head.h"
#include "platform/mii_maker.h"
#include "platform/alert_sound.h"
#include "platform/discord_presence.h"
#include "platform/save_import.h"
#include "platform/shader_cache.h"
#include "platform/texture_packs.h"
#include "platform/session_log.h"
#include "platform/gamepad_bindings.h"
#include "platform/graphics_cards.h"
#include "platform/gyro_pointer.h"
#include "platform/key_bindings.h"
#include "platform/launcher_reopen.h"
#include "platform/live_watch.h"
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
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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
    if (std::string_view(mscharged::build::channel) == "test")
        return texts::T("launcher.window.test_build_title",
                        {{"version", mscharged::build::version},
                         {"number", std::to_string(mscharged::build::pull_request)}});
    return std::string("Recharged ") + mscharged::build::version;
}

// The name desktops show for the program (task switcher, audio mixer). SDL
// forgets it at SDL_Quit, so the game sets it again after the launcher.
void SetAppName() { SDL_SetAppMetadata("Recharged", mscharged::build::version, nullptr); }

// The set a launcher started the game for: the game reads it, and so does
// the launcher it opens again afterwards (an environment variable, as
// Windows starts that one with this run's command line).
constexpr const char* kCompetitiveSetVariable = "RECHARGED_COMPETITIVE_SET";
// From both players ready to the set's game starting: 3, 2, 1.
constexpr std::uint64_t kSetCountdownMs = 3000;

void SetEnvironment(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

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
    std::snprintf(text, sizeof text, "%.1f", double(bytes) / (1024.0 * 1024.0));
    return texts::T("launcher.update.megabytes", {{"size", text}});
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

// The Wii's twelve favourite colours (RFLGetFavoriteColor, RFL_Model.c) and
// their names (in the launcher's language: LoadTableTexts), in
// RFLFavoriteColor order.
constexpr unsigned char kMiiColorRgb[12][3] = {
    {184, 64, 48},  {240, 120, 40}, {248, 216, 32}, {128, 200, 40}, {0, 116, 40},    {32, 72, 152},
    {64, 160, 216}, {232, 96, 120}, {112, 44, 168}, {72, 56, 24},   {224, 224, 224}, {24, 24, 20}};
const char* kMiiColorNames[12] = {};

ImVec4 MiiColor(int favorite_color)
{
    const auto& rgb = kMiiColorRgb[favorite_color >= 0 && favorite_color < 12 ? favorite_color : 0];
    return {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1.0f};
}

// A Mii's figure on a disc of its favourite colour (RFLGetFavoriteColor,
// RFL_Model.c), the colour the Wii puts behind its Mii icons.
void DrawMiiFigure(ImDrawList* draw, ImVec2 center, float size, int favorite_color)
{
    const ImVec4 fill = MiiColor(favorite_color);
    draw->AddCircleFilled(center, size * 0.5f, Col(fill));
    draw->AddCircle(center, size * 0.5f, Col(color::border), 0, Dp(1));
    // Dark ink on the light colours (white, yellow, yellow-green).
    const bool light = 0.2126f * fill.x + 0.7152f * fill.y + 0.0722f * fill.z > 0.6f;
    DrawIcon(draw, Icon::Person, center, size * 0.62f, Col(light ? color::window : color::text));
}

// Round swatches of `count` colours, `per_row` to a row, the chosen one
// ringed and ticked, each with its name as a tooltip when `names` has them;
// true when another is chosen.
bool ColorSwatches(const char* id, int* chosen, int count, const std::function<ImVec4(int)>& colour,
                   const char* const* names, float size, int per_row, int first = 0)
{
    bool changed = false;
    auto* draw = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    for (int n = 0; n < count; ++n)
    {
        const int c = first + n;
        if (n % per_row) ImGui::SameLine(0, Dp(10));
        ImGui::PushID(c);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##swatch", {size, size}))
        {
            changed = *chosen != c;
            *chosen = c;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (names) ImGui::SetTooltip("%s", names[n]);
        }
        const ImVec4 fill = colour(c);
        const ImVec2 center{at.x + size * 0.5f, at.y + size * 0.5f};
        const float selected = Animate(ImGui::GetID("##selected"), *chosen == c, 14.0f);
        draw->AddCircleFilled(center, size * 0.5f - Dp(3) * selected, Col(fill));
        draw->AddCircle(center, size * 0.5f - Dp(3) * selected, Col(color::border, hovered ? 1.0f : 0.7f), 0, Dp(1));
        if (selected > 0.0f) draw->AddCircle(center, size * 0.5f, Col(color::accent, selected), 0, Dp(2));
        if (*chosen == c)
        {
            const bool light = 0.2126f * fill.x + 0.7152f * fill.y + 0.0722f * fill.z > 0.6f;
            DrawIcon(draw, Icon::Check, center, size * 0.42f, Col(light ? color::window : color::text));
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// The Wii's twelve favourite colours as swatches (ColorSwatches).
bool MiiColorSwatches(const char* id, int* chosen, float size, int per_row)
{
    return ColorSwatches(id, chosen, 12, MiiColor, kMiiColorNames, size, per_row);
}

// The Mii editor's rows, each a value of the face (kMiiFaceFields): its
// parts as thumbnails of the head with each of them, colour swatches, a
// slider (Height: higher to the right, where the value grows downwards), or
// a switch; and the Mii's name.
enum class MiiControl { Parts, Colors, Slider, Height, Toggle, Name };
// The square of the head a part's thumbnails show (centre and half its
// side, 0..1 of the head picture).
struct MiiCrop
{
    float x = 0.5f, y = 0.5f, half = 0.5f;
};
struct MiiEditorRow
{
    const char* label;
    const char* field;
    MiiControl control;
    MiiCrop crop = {};
    const char* needs = nullptr; // a part this row changes, off while it is none
};
struct MiiEditorSection
{
    const char* key; // the same in every language ("facial_hair"; development captures pick by it)
    const char* name;
    std::vector<MiiEditorRow> rows;
};

// The sections in the launcher's language (LoadTableTexts makes them).
std::vector<MiiEditorSection> g_mii_editor_sections;
const std::vector<MiiEditorSection>& MiiEditorSections() { return g_mii_editor_sections; }

std::vector<MiiEditorSection> MakeMiiEditorSections()
{
    using C = MiiControl;
    const MiiCrop face{0.5f, 0.5f, 0.42f}, hair{0.5f, 0.49f, 0.44f}, brows{0.5f, 0.43f, 0.2f},
        eyes{0.5f, 0.47f, 0.2f}, nose{0.5f, 0.55f, 0.17f}, mouth{0.5f, 0.63f, 0.19f}, glasses{0.5f, 0.49f, 0.27f},
        mustache{0.5f, 0.61f, 0.19f}, beard{0.5f, 0.62f, 0.31f};
    // The labels several sections share.
    const char* type = texts::T("launcher.mii_parts.rows.type");
    const char* colour = texts::T("launcher.mii_parts.rows.colour");
    const char* size = texts::T("launcher.mii_parts.rows.size");
    const char* rotation = texts::T("launcher.mii_parts.rows.rotation");
    const char* spacing = texts::T("launcher.mii_parts.rows.spacing");
    const char* height = texts::T("launcher.mii_parts.rows.height");
    return {
        {"face", texts::T("launcher.mii_parts.sections.face"),
         {{texts::T("launcher.mii_parts.rows.shape"), "face_type", C::Parts, face},
          {texts::T("launcher.mii_parts.rows.skin"), "face_color", C::Colors},
          {texts::T("launcher.mii_parts.rows.makeup"), "face_texture", C::Parts, face}}},
        {"hair", texts::T("launcher.mii_parts.sections.hair"),
         {{texts::T("launcher.mii_parts.rows.style"), "hair_type", C::Parts, hair}, {colour, "hair_color", C::Colors},
          {texts::T("launcher.mii_parts.rows.parted"), "hair_flip", C::Toggle}}},
        {"eyebrows", texts::T("launcher.mii_parts.sections.eyebrows"),
         {{type, "eyebrow_type", C::Parts, brows}, {colour, "eyebrow_color", C::Colors},
          {size, "eyebrow_scale", C::Slider}, {rotation, "eyebrow_rotate", C::Slider},
          {spacing, "eyebrow_x", C::Slider}, {height, "eyebrow_y", C::Height}}},
        {"eyes", texts::T("launcher.mii_parts.sections.eyes"),
         {{type, "eye_type", C::Parts, eyes}, {colour, "eye_color", C::Colors}, {size, "eye_scale", C::Slider},
          {rotation, "eye_rotate", C::Slider}, {spacing, "eye_x", C::Slider}, {height, "eye_y", C::Height}}},
        {"nose", texts::T("launcher.mii_parts.sections.nose"),
         {{type, "nose_type", C::Parts, nose}, {size, "nose_scale", C::Slider}, {height, "nose_y", C::Height}}},
        {"mouth", texts::T("launcher.mii_parts.sections.mouth"),
         {{type, "mouth_type", C::Parts, mouth}, {colour, "mouth_color", C::Colors}, {size, "mouth_scale", C::Slider},
          {height, "mouth_y", C::Height}}},
        {"glasses", texts::T("launcher.mii_parts.sections.glasses"),
         {{type, "glasses_type", C::Parts, glasses}, {colour, "glasses_color", C::Colors, {}, "glasses_type"},
          {size, "glasses_scale", C::Slider, {}, "glasses_type"},
          {height, "glasses_y", C::Height, {}, "glasses_type"}}},
        {"facial_hair", texts::T("launcher.mii_parts.sections.facial_hair"),
         {{texts::T("launcher.mii_parts.rows.mustache"), "mustache_type", C::Parts, mustache},
          {texts::T("launcher.mii_parts.rows.beard"), "beard_type", C::Parts, beard},
          {colour, "beard_color", C::Colors},
          {texts::T("launcher.mii_parts.rows.mustache_size"), "beard_scale", C::Slider, {}, "mustache_type"},
          {texts::T("launcher.mii_parts.rows.mustache_height"), "beard_y", C::Height, {}, "mustache_type"}}},
        {"mole", texts::T("launcher.mii_parts.sections.mole"),
         {{texts::T("launcher.mii_parts.rows.mole"), "mole_type", C::Toggle},
          {size, "mole_scale", C::Slider, {}, "mole_type"},
          {texts::T("launcher.mii_parts.rows.across"), "mole_x", C::Slider, {}, "mole_type"},
          {height, "mole_y", C::Height, {}, "mole_type"}}},
        {"name_colour", texts::T("launcher.mii_parts.sections.name_colour"),
         {{texts::T("launcher.mii_parts.rows.name"), nullptr, C::Name},
          {texts::T("launcher.mii_parts.rows.favourite_colour"), "favorite_color", C::Colors}}},
    };
}

const mscharged::platform::mii::MiiFaceField& FaceField(std::string_view name)
{
    for (const auto& field : mscharged::platform::mii::kMiiFaceFields)
        if (name == field.name) return field;
    throw std::logic_error("No Mii face field " + std::string(name));
}

bool SameFace(const mscharged::platform::mii::MiiFace& a, const mscharged::platform::mii::MiiFace& b)
{
    for (const auto& field : mscharged::platform::mii::kMiiFaceFields)
        if (a.*field.value != b.*field.value) return false;
    return true;
}

// Pills that wrap within `width`, the selected one lit; true when another
// is chosen.
bool Pills(const char* id, const std::vector<const char*>& labels, int* selected, float width, ImFont* font,
           float height)
{
    bool changed = false;
    auto* draw = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    const float gap = Dp(6);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = 0.0f, y = 0.0f;
    for (int n = 0; n < int(labels.size()); ++n)
    {
        const ImVec2 text = TextSize(font, labels[std::size_t(n)]);
        const float pill = text.x + Dp(26);
        if (x > 0.0f && x + pill > width)
        {
            x = 0.0f;
            y += height + gap;
        }
        ImGui::SetCursorScreenPos({start.x + x, start.y + y});
        ImGui::PushID(n);
        if (ImGui::InvisibleButton("##pill", {pill, height}) && *selected != n)
        {
            *selected = n;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float hover = Animate(ImGui::GetItemID(), hovered, 14.0f);
        const float on = Animate(ImGui::GetID("##on"), *selected == n, 14.0f);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        draw->AddRectFilled(min, max, Col(Mix(Mix(color::field, color::card_hover, hover), color::accent_dark, on)),
                            height * 0.5f);
        draw->AddRect(min, max, Col(Mix(color::border, color::accent, on), 0.6f + 0.4f * on), height * 0.5f, 0,
                      Dp(1));
        DrawLabel(draw, font, {min.x + (pill - text.x) * 0.5f, min.y + (height - text.y) * 0.5f},
                  Col(Mix(Mix(color::muted, color::text, hover), color::accent_hi, on)), labels[std::size_t(n)]);
        ImGui::PopID();
        x += pill + gap;
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({width, y + height});
    ImGui::PopID();
    return changed;
}

// A profile name as the server takes it (POST /api/v1/sign-in/mii): `name`
// without spaces around it; valid for 1 to 32 characters, none a control
// character.
std::string TrimmedProfileName(const std::string& name)
{
    const auto first = name.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return name.substr(first, name.find_last_not_of(" \t") - first + 1);
}

bool ValidProfileName(const std::string& name)
{
    const std::string trimmed = TrimmedProfileName(name);
    std::size_t characters = 0;
    for (std::size_t n = 0; n < trimmed.size(); ++characters)
    {
        const auto decoded = mscharged::platform::NextCodepoint(trimmed, n);
        if (!decoded) return false;
        const char32_t c = *decoded;
        // Control characters, and the invisible format ones (soft hyphen,
        // zero-width and direction marks, word joiners, BOM, tags).
        if (c < 0x20 || (c >= 0x7F && c < 0xA0) || c == 0xAD || (c >= 0x600 && c <= 0x605) || c == 0x61C
            || c == 0x6DD || c == 0x70F || c == 0x180E || (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E)
            || (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB)
            || (c >= 0xE0000 && c <= 0xE007F))
            return false;
    }
    return characters >= 1 && characters <= 32;
}

// In the sidebar's order. The pages of a group (kNavGroups) follow each other.
// The leaderboards and events are the Recharged Game Menu's (its Ranked and
// Events tabs); the settings that change while the game runs (the display
// and sound, the gameplay options, the rumble...) are its OPTIONS.
enum Page { PagePlay, PageOnlineSettings, PageRules, PageSidekicks, PageGame, PageGraphics,
            PageControls, PageAdvanced, PageExtras, PageReplays, PageAbout, PageCount };
constexpr const char* page_ids[] = {"play",     "online-settings", "rules",  "sidekicks", "game", "graphics",
                                    "controls", "advanced",        "extras", "replays",   "about"};
constexpr Icon page_icons[] = {Icon::Play,     Icon::Person,  Icon::Clipboard, Icon::Pitch,   Icon::Disc, Icon::Display,
                               Icon::Keyboard, Icon::Sliders, Icon::Target,    Icon::Archive, Icon::Info};
static_assert(std::size(page_ids) == PageCount && std::size(page_icons) == PageCount);
// --page's names of the pages that were merged into others.
constexpr std::pair<const char*, int> kOldPageIds[] = {{"display", PageGraphics}, {"audio", PageGame},
                                                       {"gameplay", PageExtras}};

// A page's name in the sidebar.
const char* PageName(int page)
{
    switch (page)
    {
    case PagePlay: return texts::T("launcher.pages.play");
    case PageGraphics: return texts::T("launcher.pages.graphics");
    case PageGame: return texts::T("launcher.pages.game");
    case PageOnlineSettings: return texts::T("launcher.pages.player_profile");
    case PageRules: return texts::T("launcher.pages.match_rules");
    case PageSidekicks: return texts::T("launcher.pages.sidekicks");
    case PageReplays: return texts::T("launcher.pages.replays");
    case PageControls: return texts::T("launcher.pages.controls");
    case PageExtras: return texts::T("launcher.pages.extras");
    case PageAdvanced: return texts::T("launcher.pages.advanced");
    case PageAbout: return texts::T("launcher.pages.about");
    default: return "";
    }
}

// The sidebar's groups of pages: one row that opens the last of its pages
// shown, and shows their rows while one of them is open.
struct NavGroupInfo
{
    Icon icon;
    int first;
    int count;
};
constexpr NavGroupInfo kNavGroups[] = {{Icon::Chat, PageOnlineSettings, 3}, {Icon::Gear, PageGraphics, 3}};
// A group's name in the sidebar (kNavGroups order).
const char* NavGroupName(int group)
{
    return group == 0 ? texts::T("launcher.pages.online") : group == 1 ? texts::T("launcher.pages.configuration") : "";
}
constexpr int NavGroupOf(int page)
{
    for (int group = 0; group < int(std::size(kNavGroups)); ++group)
        if (page >= kNavGroups[group].first && page < kNavGroups[group].first + kNavGroups[group].count) return group;
    return -1;
}
// Pages that share a group's row, and the most pages one group has.
constexpr int NavGroupedRows()
{
    int rows = 0;
    for (const auto& group : kNavGroups) rows += group.count - 1;
    return rows;
}
constexpr int NavGroupMostPages()
{
    int most = 0;
    for (const auto& group : kNavGroups) most = group.count > most ? group.count : most;
    return most;
}
static_assert(NavGroupOf(PageOnlineSettings) == 0 && NavGroupOf(PageRules) == 0 && NavGroupOf(PageSidekicks) == 0
              && NavGroupOf(PageReplays) == -1 && NavGroupOf(PageGame) == -1 && NavGroupOf(PageExtras) == -1
              && NavGroupOf(PageGraphics) == 1 && NavGroupOf(PageControls) == 1 && NavGroupOf(PageAdvanced) == 1);
constexpr bool OnlinePage(int page) { return NavGroupOf(page) == 0; }

// The captains (eTeamID order) and the sidekicks (eSidekickID order), as the
// game names them; Training's choices, in the order of kTrainingItems,
// kTrainingStadiums and kTrainingSkills. In the launcher's language
// (LoadTableTexts).
const char* kCaptainNames[mscharged::platform::kCaptainCount] = {};
const char* kSidekickNames[mscharged::platform::kSidekickCount] = {};
const char* kTrainingItemNames[mscharged::kTrainingItems.size()] = {};
const char* kStadiumNames[mscharged::kTrainingStadiums.size()] = {};
const char* kSkillNames[mscharged::kTrainingSkills.size()] = {};
static_assert(std::size(kCaptainNames) == mscharged::kTrainingCaptains.size());

// Where the Teams page's pitch puts each player of the team, which attacks to
// the right: x from its own goal line (0) to the other one (1), y from its
// left touchline (0) to its right one (1). kSidekickSpots is by slot, the
// order of the lineup in the save; which slot plays where is yet to be
// checked in the game.
struct PitchSpot
{
    const char* role; // in the launcher's language (LoadTableTexts)
    float x, y;
};
PitchSpot kSidekickSpots[] = {{"", 0.44f, 0.22f}, {"", 0.44f, 0.78f}, {"", 0.27f, 0.50f}};
PitchSpot kCaptainSpot{"", 0.76f, 0.50f}, kGoalieSpot{"", 0.075f, 0.50f};
static_assert(std::size(kSidekickSpots) == mscharged::platform::kSidekickSlots);

// `names` over a table of the same size.
template <std::size_t Size>
void SetNames(const char* (&table)[Size], const char* const (&names)[Size])
{
    std::copy(names, names + Size, table);
}

// The tables above in the launcher's language: when it loads its texts
// (LoadTexts), at the start and when the menu language changes.
void LoadTableTexts()
{
    SetNames(kMiiColorNames,
             {texts::T("launcher.mii_colors.red"), texts::T("launcher.mii_colors.orange"),
              texts::T("launcher.mii_colors.yellow"), texts::T("launcher.mii_colors.light_green"),
              texts::T("launcher.mii_colors.green"), texts::T("launcher.mii_colors.blue"),
              texts::T("launcher.mii_colors.light_blue"), texts::T("launcher.mii_colors.pink"),
              texts::T("launcher.mii_colors.purple"), texts::T("launcher.mii_colors.brown"),
              texts::T("launcher.mii_colors.white"), texts::T("launcher.mii_colors.black")});
    g_mii_editor_sections = MakeMiiEditorSections();
    SetNames(kCaptainNames,
             {texts::T("launcher.names.captains.mario"), texts::T("launcher.names.captains.bowser"),
              texts::T("launcher.names.captains.daisy"), texts::T("launcher.names.captains.donkey_kong"),
              texts::T("launcher.names.captains.luigi"), texts::T("launcher.names.captains.peach"),
              texts::T("launcher.names.captains.waluigi"), texts::T("launcher.names.captains.wario"),
              texts::T("launcher.names.captains.yoshi"), texts::T("launcher.names.captains.bowser_jr"),
              texts::T("launcher.names.captains.diddy_kong"), texts::T("launcher.names.captains.petey_piranha")});
    SetNames(kSidekickNames,
             {texts::T("launcher.names.sidekicks.toad"), texts::T("launcher.names.sidekicks.koopa"),
              texts::T("launcher.names.sidekicks.hammer_bro"), texts::T("launcher.names.sidekicks.birdo"),
              texts::T("launcher.names.sidekicks.boo"), texts::T("launcher.names.sidekicks.dry_bones"),
              texts::T("launcher.names.sidekicks.monty_mole"), texts::T("launcher.names.sidekicks.shy_guy")});
    SetNames(kTrainingItemNames,
             {texts::T("launcher.names.items.none"), texts::T("launcher.names.items.green_shell"),
              texts::T("launcher.names.items.green_shell_3"), texts::T("launcher.names.items.green_shell_5"),
              texts::T("launcher.names.items.green_shell_giant"), texts::T("launcher.names.items.red_shell"),
              texts::T("launcher.names.items.red_shell_3"), texts::T("launcher.names.items.red_shell_giant"),
              texts::T("launcher.names.items.spiny_shell"), texts::T("launcher.names.items.spiny_shell_3"),
              texts::T("launcher.names.items.spiny_shell_5"), texts::T("launcher.names.items.spiny_shell_giant"),
              texts::T("launcher.names.items.freeze_shell"), texts::T("launcher.names.items.freeze_shell_3"),
              texts::T("launcher.names.items.freeze_shell_5"), texts::T("launcher.names.items.freeze_shell_giant"),
              texts::T("launcher.names.items.banana"), texts::T("launcher.names.items.banana_3"),
              texts::T("launcher.names.items.banana_5"), texts::T("launcher.names.items.banana_giant"),
              texts::T("launcher.names.items.bob_omb"), texts::T("launcher.names.items.bob_omb_3"),
              texts::T("launcher.names.items.bob_omb_5"), texts::T("launcher.names.items.bob_omb_giant"),
              texts::T("launcher.names.items.chain_chomp"), texts::T("launcher.names.items.mushroom"),
              texts::T("launcher.names.items.star"), texts::T("launcher.names.items.captains_ability")});
    SetNames(kStadiumNames,
             {texts::T("launcher.names.stadiums.battle_dome"), texts::T("launcher.names.stadiums.bowser_stadium"),
              texts::T("launcher.names.stadiums.crater_field"), texts::T("launcher.names.stadiums.crystal_canyon"),
              texts::T("launcher.names.stadiums.the_dump"), texts::T("launcher.names.stadiums.galactic_stadium"),
              texts::T("launcher.names.stadiums.konga_coliseum"), texts::T("launcher.names.stadiums.the_lava_pit"),
              texts::T("launcher.names.stadiums.pipeline_central"),
              texts::T("launcher.names.stadiums.stormship_stadium"), texts::T("launcher.names.stadiums.the_palace"),
              texts::T("launcher.names.stadiums.thunder_island"), texts::T("launcher.names.stadiums.the_underground"),
              texts::T("launcher.names.stadiums.the_vice"), texts::T("launcher.names.stadiums.the_wastelands"),
              texts::T("launcher.names.stadiums.the_sand_tomb"), texts::T("launcher.names.stadiums.the_classroom")});
    SetNames(kSkillNames,
             {texts::T("launcher.names.skills.training"), texts::T("launcher.names.skills.rookie"),
              texts::T("launcher.names.skills.professional"), texts::T("launcher.names.skills.superstar"),
              texts::T("launcher.names.skills.legend"), texts::T("launcher.names.skills.megastriker")});
    kSidekickSpots[0].role = texts::T("launcher.names.positions.left_wing");
    kSidekickSpots[1].role = texts::T("launcher.names.positions.right_wing");
    kSidekickSpots[2].role = texts::T("launcher.names.positions.defender");
    kCaptainSpot.role = texts::T("launcher.names.positions.captain");
    kGoalieSpot.role = texts::T("launcher.names.positions.goalie");
}

// The sparks that crackle over the Recharged logo now and then are off for
// now: the logo stays still. Set to true to bring them back.
constexpr bool kLogoSparks = false;

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
    bool direct_game = false;  // --disc or --training without a runtime mode: the game, to play
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
    return texts::T("launcher.image.region_revision",
                    {{"region", DiscRegionName(info.game_id)}, {"revision", std::to_string(unsigned(info.revision))}});
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
enum class FolderPurpose { SaveFolder, DolphinSave, DolphinMiis, GameFolder, TextureFolder, ProfileImport };

// What waits for an input to bind it, one at a time: a keyboard action's key
// (the keyboard view), a hotkey (the Controls page's Hotkeys card) or a
// gamepad action's input (the gamepad view). Leaving the page or view,
// Revert, Escape and a click elsewhere stop the wait.
struct InputCapture
{
    enum class Kind { None, Key, Hotkey, Pad };
    Kind kind = Kind::None;
    int action = -1; // the action (a HotkeyIndex for a hotkey)
    int slot = -1;   // which of its bindings (none for a hotkey)

    bool Waits(Kind wanted) const { return kind == wanted; }
    bool WaitsFor(Kind wanted, int wanted_action, int wanted_slot = -1) const
    {
        return kind == wanted && action == wanted_action && slot == wanted_slot;
    }
    void Wait(Kind wanted, int wanted_action, int wanted_slot = -1)
    {
        kind = wanted;
        action = wanted_action;
        slot = wanted_slot;
    }
    void Cancel() { *this = InputCapture{}; }
};
// What the import prompt copies from Dolphin.
enum class ImportKind { Save, Miis };

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

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;

// An RGBA32 image at half the size, each pixel the alpha-weighted mean of four:
// linear filtering alone flickers when it shrinks an image several times.
SurfacePtr HalfSize(const SDL_Surface* source)
{
    SurfacePtr half(SDL_CreateSurface(source->w / 2, source->h / 2, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    if (!half) return half;
    for (int y = 0; y < half->h; ++y)
    {
        const auto* top = static_cast<const Uint8*>(source->pixels) + std::size_t(y) * 2 * std::size_t(source->pitch);
        const auto* bottom = top + source->pitch;
        auto* out = static_cast<Uint8*>(half->pixels) + std::size_t(y) * std::size_t(half->pitch);
        for (int x = 0; x < half->w; ++x, out += 4)
        {
            const Uint8* p[] = {top + x * 8, top + x * 8 + 4, bottom + x * 8, bottom + x * 8 + 4};
            const unsigned alpha = unsigned(p[0][3]) + p[1][3] + p[2][3] + p[3][3];
            for (int c = 0; c < 3; ++c)
            {
                const unsigned sum = unsigned(p[0][c]) * p[0][3] + unsigned(p[1][c]) * p[1][3]
                    + unsigned(p[2][c]) * p[2][3] + unsigned(p[3][c]) * p[3][3];
                out[c] = Uint8(alpha ? (sum + alpha / 2) / alpha : 0);
            }
            out[3] = Uint8((alpha + 2) / 4);
        }
    }
    return half;
}

class Launcher
{
public:
    const std::optional<ResolvedLaunch>& FrontendLaunch() const { return frontend_launch_; }
    // An update was installed: main() starts the updated launcher.
    bool RestartRequested() const { return restart_; }
    fs::path RestartExecutable() const { return InstalledLauncher(base_); }

    ResolvedLaunch EffectiveLaunch() const { return ResolveLaunch(file_, options_.launch, &draft_); }
    // The chosen image's path, as the game gets it. Asked many times a frame:
    // computed from the draft directly (a whole ResolvedLaunch copies the
    // settings file); a --disc on the command line goes through its rules.
    std::string DiscSelection() const
    {
        if (options_.launch.disc) return PathUtf8(EffectiveLaunch().disc_path);
        return draft_.disc.empty() ? std::string() : PathUtf8(ResolveDiscPath(draft_, file_.path).lexically_normal());
    }

    ~Launcher()
    {
        wii_probe_.Close();
        remote_lights_.reset(); // a remote that still rumbles stops first
        mii_faces_.reset();     // its textures, before the renderer goes
        if (renderer_ui_ready_) ImGui_ImplSDLRenderer3_Shutdown();
        if (window_ui_ready_) ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        for (auto* pad : pads_) SDL_CloseGamepad(pad);
        // A running library scan stops at its next image; its future waits for it.
        if (library_progress_) library_progress_->cancel = true;
        for (auto& [id, texture] : banners_)
            if (texture) SDL_DestroyTexture(texture);
        ClearPictureTextures();
        if (header_back_) SDL_DestroyTexture(header_back_);
        if (header_front_) SDL_DestroyTexture(header_front_);
        if (header_strokes_) SDL_DestroyTexture(header_strokes_);
        for (const auto& level : logo_) SDL_DestroyTexture(level.texture);
        for (const auto& level : logo_sizzle_) SDL_DestroyTexture(level.texture);
        if (aurora_logo_) SDL_DestroyTexture(aurora_logo_);
        if (flags_) SDL_DestroyTexture(flags_);
        alert_sound_.reset(); // before SDL's audio goes
        set_presence_.reset();
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
        // The WFC server's Watch button opens this program (recharged:// links).
        if (!capture_)
            if (const auto line = platform::RegisterWatchLinks(InstalledLauncher(base_)); !line.empty())
                platform::SessionLogLine(line);
        // --replay: the launcher starts it once it found the image it needs;
        // other starts from this launcher play as usual.
        if (options_.launch.replay)
        {
            if (!capture_ && config_ok_) AskReplay(fs::absolute(*options_.launch.replay));
            options_.launch.replay.reset();
        }
        // Development captures of a replay's match stats (--page replays):
        // RECHARGED_CAPTURE_STATS names the replay.
        if (const char* stats = capture_ ? std::getenv("RECHARGED_CAPTURE_STATS") : nullptr; stats && *stats)
            OpenReplayStats(PathFromUtf8(stats));
        StartUpdates();
        StartCompetitive();
        // Back from a set's game: that set's game does not start again by itself.
        if (const char* set = std::getenv(kCompetitiveSetVariable); set && *set && !capture_)
            competitive_started_go_ = std::strtoll(set, nullptr, 10);
        StartPlayersOnline();
        StartServerMessage();
#if !defined(__APPLE__)
        // The PC's graphics cards, for Advanced -> Graphics card and the log.
        graphics_cards_scan_.Start(platform::ListGraphicsCards);
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
        StartMiiFaces();
        metrics_ = CurrentMetrics();
        ApplyWindowMetrics(true);

        const auto resources = base_ / "assets/launcher";
        // The header's three layers, the same proportions: the scene, gold strokes
        // bursting behind Mario, and Mario with the ball over them.
        for (const auto& [texture, name] :
             {std::pair{&header_back_, "header_back.png"}, std::pair{&header_strokes_, "header_strokes.png"},
              std::pair{&header_front_, "header_front.png"}})
        {
            SurfacePtr surface(SDL_LoadPNG(PathUtf8(resources / name).c_str()), SDL_DestroySurface);
            Require(surface != nullptr, "Cannot load launcher header; rebuild to restore resources");
            header_width_ = float(surface->w);
            header_height_ = float(surface->h);
            *texture = SDL_CreateTextureFromSurface(renderer_, surface.get());
            Require(*texture != nullptr, "Cannot create header texture");
            SDL_SetTextureScaleMode(*texture, SDL_SCALEMODE_LINEAR);
        }
        SDL_SetTextureBlendMode(header_strokes_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(header_front_, SDL_BLENDMODE_BLEND);
        // The Recharged logo and the same logo with sparks, which sizzles over
        // it, each with halved copies down to 128 pixels wide for small sizes.
        const auto load_levels = [&](const char* name, std::vector<LogoLevel>& levels) {
            SurfacePtr level(SDL_LoadPNG(PathUtf8(resources / name).c_str()), SDL_DestroySurface);
            if (level) level.reset(SDL_ConvertSurface(level.get(), SDL_PIXELFORMAT_RGBA32));
            Require(level != nullptr, "Cannot load launcher logo; rebuild to restore resources");
            while (level)
            {
                SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, level.get());
                Require(texture != nullptr, "Cannot create logo texture");
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                levels.push_back({texture, float(level->w), float(level->h)});
                if (level->w < 256) break;
                level = HalfSize(level.get());
            }
        };
        load_levels("logo.png", logo_);
        logo_aspect_ = logo_.front().width / logo_.front().height;
        // tools/launcher_art.py cuts both out alike; another picture is left out.
        if (kLogoSparks) load_levels("logo_sizzle.png", logo_sizzle_);
        if (!logo_sizzle_.empty() && (logo_sizzle_.front().width != logo_.front().width ||
                                      logo_sizzle_.front().height != logo_.front().height))
        {
            for (const auto& level : logo_sizzle_) SDL_DestroyTexture(level.texture);
            logo_sizzle_.clear();
        }
        // Aurora's logo (from its pinned source) is optional decoration.
        if (auto logo = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
                SDL_LoadPNG(PathUtf8(resources / "aurora.png").c_str()), SDL_DestroySurface))
        {
            aurora_logo_width_ = float(logo->w);
            aurora_logo_height_ = float(logo->h);
            aurora_logo_ = SDL_CreateTextureFromSurface(renderer_, logo.get());
            if (aurora_logo_) SDL_SetTextureScaleMode(aurora_logo_, SDL_SCALEMODE_LINEAR);
        }
        // The flags beside players' names (overlay/flags.h); without them the
        // Flag row goes by names alone.
        if (SurfacePtr flags{SDL_LoadPNG(PathUtf8(base_ / "assets/flags/flags.png").c_str()), SDL_DestroySurface})
        {
            flags_ = SDL_CreateTextureFromSurface(renderer_, flags.get());
            if (flags_) SDL_SetTextureScaleMode(flags_, SDL_SCALEMODE_LINEAR);
        }
        // The welcome guide opens at the first start (never in development captures).
        LoadWelcome(resources / "welcome.md");
        welcome_pending_ = config_ok_ && file_.settings.show_welcome && !welcome_.empty() && !capture_;
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
        // Frames in a row with nothing on its way: a capture waits for three, so
        // that cards sized by their contents have measured them.
        unsigned settled_frames = 0;
        const unsigned smoke_frames = unsigned(PageCount) * 4 + 2;
        while (!quit)
        {
            const Uint64 frame_start = SDL_GetTicks();
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                const bool capturing = input_capture_.Waits(InputCapture::Kind::Key) || input_capture_.Waits(InputCapture::Kind::Hotkey);
                if (!capturing || (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_TEXT_INPUT))
                    ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                    && event.window.windowID == SDL_GetWindowID(window_)))
                    close_requested_ = true;
                if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
                    RefreshGamepad();
                if (event.type == SDL_EVENT_DISPLAY_ADDED || event.type == SDL_EVENT_DISPLAY_REMOVED)
                    RefreshDisplays();
                // A dropped replay plays; on the Extras page a dropped texture
                // pack is added; anything else is a game image or folder, as
                // on the other pages.
                // A watch link the system hands over (macOS: the Watch button
                // of the WFC server's dashboard): its ticket plays like a replay.
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data && IsWatchLink(event.drop.data))
                {
                    std::string error;
                    const auto ticket = WriteWatchLinkTicket(event.drop.data, fs::temp_directory_path(), error);
                    if (ticket.empty())
                        Notice(texts::T("launcher.replay.unreadable", {{"error", error}}), NoticeKind::Error);
                    else if (config_ok_)
                        AskReplay(ticket);
                }
                else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data
                    && replay::IsReplayFile(PathFromUtf8(event.drop.data)))
                {
                    if (config_ok_) AskReplay(PathFromUtf8(event.drop.data));
                }
                else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data
                    && !(page_ == PageExtras && config_ok_ && pack_library_.Add(PathFromUtf8(event.drop.data), TextureFolder())))
                    SelectDisc(event.drop.data);
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && input_capture_.Waits(InputCapture::Kind::Key))
                    CaptureKey(event.key.scancode);
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && input_capture_.Waits(InputCapture::Kind::Hotkey))
                    CaptureHotkey(event.key.scancode, event.key.mod);
                // Mouse buttons can take a key's place (#62); a left click is
                // taken on the waiting keycap itself (KeyboardView), elsewhere
                // it stops the wait.
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && input_capture_.Waits(InputCapture::Kind::Key)
                    && event.button.button != SDL_BUTTON_LEFT
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
            if (input_capture_.Waits(InputCapture::Kind::Pad) || pad_capture_release_) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
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
            // A name with characters the fonts lack arrived: they are added.
            else if (platform::NameGlyphsVersion() != NameGlyphsBuilt())
                RebuildScale();
            SetUiScale(metrics_.ui);
            UpdateMiiFaces();
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
            SDL_SetRenderDrawColorFloat(renderer_, color::window.x, color::window.y, color::window.z, 1.0f);
            SDL_RenderClear(renderer_);
            RenderHero();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
            const bool update_settled = update_preview_ || (!UpdateBusy() && !(update_capture_install_ && update_.offer));
            const bool settled = !disc_check_.Busy() && !search_.Busy() && !library_scan_.Busy() && !candidate_.Busy()
                && !pack_library_.Scanning() && update_settled && CompetitiveSettled() && !server_message_fetch_
                && (!mii_faces_ || mii_faces_->Settled()) && !picture_job_.Busy() && !replay_stats_job_.Busy();
            settled_frames = settled ? settled_frames + 1 : 0;
            if (!options_.screenshot.empty() && frame >= 12 && (settled_frames >= 3 || frame > 600))
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
        if (options_.smoke_test && mii_editor_frames_)
            std::cout << "The Mii editor was drawn in " << mii_editor_frames_ << " frames.\n";
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
        // ImGui then paints its gradients and text over it. Each time the
        // Play page opens, the artwork makes an entrance, the shot coming in:
        // it fades in from the page colour while its layers glide from
        // the top right towards the bottom left and grow about Mario's face,
        // Mario and the ball further than the gold strokes behind them and
        // those further than the scene, slowing down until
        // Mario rests in that crop. He starts smaller, so his layer starts
        // shifted up and right at least as far as keeps its top and right
        // edges, where the art cuts his raised hand and his fist, beyond the
        // window's (its other edges are transparent). The scene, which must
        // cover every edge, grows from a little closer than the crop instead.
        if (!hero_visible_ || hero_.w <= 0 || hero_.h <= 0)
        {
            hero_shown_ = false;
            return;
        }
        constexpr float glide_seconds = 8.125f, fade_seconds = 0.8f;
        const float scale = std::max(hero_.w / header_width_, hero_.h / header_height_);
        const float width = header_width_ * scale, height = header_height_ * scale;
        const SDL_FRect art{hero_.x + (hero_.w - width) * 0.5f, hero_.y + (hero_.h - height) * 0.40f, width, height};
        // The entrance's own clock: a slow frame (the first ones upload the
        // fonts) holds it back rather than skipping part of it.
        if (capture_) hero_time_ = glide_seconds;
        else if (!hero_shown_) hero_time_ = 0;
        else hero_time_ += std::min(ImGui::GetIO().DeltaTime, 1.0f / 30);
        hero_shown_ = true;
        // The part of the glide still to come, easing out (quintic) from 1 to 0.
        const float rest = 1 - std::min(hero_time_ / glide_seconds, 1.0f);
        const float remaining = rest * rest * rest * rest * rest;
        // Mario's face, 76 % across and 37 % down the artwork.
        const float cx = art.x + art.w * 0.76f, cy = art.y + art.h * 0.37f;
        // A layer grown by `grow` about the face and shifted `dx` right (and 0.6 dx up).
        const auto layer = [&](SDL_Texture* texture, float grow, float dx) {
            const SDL_FRect target{cx + (art.x - cx) * grow + dx, cy + (art.y - cy) * grow - dx * 0.6f,
                                   art.w * grow, art.h * grow};
            SDL_RenderTexture(renderer_, texture, nullptr, &target);
        };
        constexpr float front_zoom = 0.08f;
        const float lift = std::max({0.025f * hero_.w, front_zoom * (art.x + art.w - cx),
                                     front_zoom * (cy - art.y) / 0.6f});
        const SDL_Rect clip{int(std::floor(hero_.x)), int(std::floor(hero_.y)), int(std::ceil(hero_.w)),
                            int(std::ceil(hero_.h))};
        SDL_SetRenderClipRect(renderer_, &clip);
        layer(header_back_, 1.025f + 0.04f * (1 - remaining), 0.010f * hero_.w * remaining);
        // The strokes, further back than Mario, glide the same way but less (both the
        // growth and the shift, so that their top and right edges stay out too).
        constexpr float strokes_depth = 0.6f;
        layer(header_strokes_, 1 - front_zoom * strokes_depth * remaining, lift * strokes_depth * remaining);
        layer(header_front_, 1 - front_zoom * remaining, lift * remaining);
        // The fade: the page colour over the whole picture, not each layer
        // faded on its own, which would show the scene through Mario.
        const float shown = std::min(hero_time_ / fade_seconds, 1.0f);
        if (shown < 1)
        {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColorFloat(renderer_, color::window.x, color::window.y, color::window.z,
                                        1 - shown * shown * (3 - 2 * shown));
            SDL_RenderFillRect(renderer_, &hero_);
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
    }

    // The logo with its top left corner at `pos`, `height` tall, the sparked
    // logo sizzling over it (with kLogoSparks); returns its width.
    float DrawLogo(ImDrawList* draw, ImVec2 pos, float height)
    {
        const float width = height * logo_aspect_;
        // The smallest copy still as wide as the pixels it covers.
        std::size_t level = 0;
        for (std::size_t i = 0; i < logo_.size(); ++i)
            if (logo_[i].width >= width * metrics_.density) level = i;
        const ImVec2 max{pos.x + width, pos.y + height};
        draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo_[level].texture)), pos, max);
        if (const float sizzle = Sizzle(); sizzle > 0.0f && level < logo_sizzle_.size())
            draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo_sizzle_[level].texture)),
                           pos, max, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, int(sizzle * 255.0f + 0.5f)));
        return width;
    }

    // How much of the sparked logo shows over the plain one now. At random
    // moments, 1.5 to 6 seconds apart, the sparks crackle for 0.35 to 0.9
    // seconds (now and then striking again at once), flickering to another
    // strength every few hundredths of a second, rising fast and fading out.
    // Nothing in screenshots, so that they stay the same, nor without kLogoSparks.
    float Sizzle()
    {
        if (capture_ || logo_sizzle_.empty()) return 0.0f;
        const double now = ImGui::GetTime();
        std::uniform_real_distribution<double> random(0.0, 1.0);
        if (now >= sizzle_.next)
        {
            sizzle_.start = now;
            sizzle_.length = 0.35 + 0.55 * random(sizzle_.rng);
            const double pause = random(sizzle_.rng) < 0.3 ? 0.12 + 0.25 * random(sizzle_.rng)
                                                             : 1.5 + 4.5 * random(sizzle_.rng);
            sizzle_.next = now + sizzle_.length + pause;
            sizzle_.flicker_until = now;
        }
        const double t = now - sizzle_.start;
        if (t >= sizzle_.length) return 0.0f;
        if (now >= sizzle_.flicker_until)
        {
            sizzle_.flicker = float(0.25 + 0.75 * random(sizzle_.rng));
            sizzle_.flicker_until = now + 0.03 + 0.06 * random(sizzle_.rng);
        }
        const float u = float(t / sizzle_.length);
        const float fall = std::clamp((u - 0.4f) / 0.6f, 0.0f, 1.0f);
        return std::min(u / 0.12f, 1.0f) * (1 - fall * fall * (3 - 2 * fall)) * sizzle_.flicker;
    }

    // ------------------------------------------------------------ settings
    void Notice(std::string text, NoticeKind kind)
    {
        notice_ = std::move(text);
        notice_kind_ = kind;
    }

    // A language folder's name the settings take ([interface] language, as
    // bootstrap/config.cpp checks it): letters, digits and hyphens, at most 16.
    static bool MenuLanguageCode(const std::string& code)
    {
        return !code.empty() && code.size() <= 16
            && code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-")
            == std::string::npos;
    }

    // Recharged's texts in the menu language ([interface] language): at the
    // start, before the fonts are built, and again when the setting changes
    // (the Game page, Revert). Their characters are noted for the fonts, which
    // the next frame builds again when some are new to them.
    void LoadTexts()
    {
        if (texts_language_ == draft_.interface_language) return;
        const fs::path folder = base_ / "assets" / "lang";
        const bool first = !texts_language_;
        // The languages there are, each by its own name (read once: a new
        // folder shows at the next start).
        if (first)
            for (auto& choice : texts::Languages(folder))
                if (MenuLanguageCode(choice.code))
                {
                    platform::NoteNameGlyphs(choice.name);
                    menu_languages_.push_back(std::move(choice));
                }
        texts_language_ = draft_.interface_language;
        const std::string loaded = texts::Load(folder, *texts_language_, pc_languages_);
        platform::NoteNameGlyphs(texts::AllTexts());
        LoadTableTexts();
        platform::SessionLogLine("Interface language: " + loaded);
        // The display list's names ("primary").
        if (!first) RefreshDisplays();
    }

    // Saves one choice at once (the update channel, the welcome seen, the Mii
    // sign-in and online Mii), without the other unsaved changes: `change`
    // goes into the draft and into the saved settings. A save that fails
    // leaves it among the unsaved changes, so the bottom bar offers it again.
    void SaveNow(const std::function<void(Settings&)>& change)
    {
        change(draft_);
        if (!config_ok_) return;
        try
        {
            Settings saved = file_.settings;
            change(saved);
            SaveConfig(file_, saved);
        }
        catch (const std::exception& error)
        {
            dirty_ = true;
            Notice(error.what(), NoticeKind::Error);
        }
    }

    void Reload()
    {
        try
        {
            file_ = LoadConfig(file_.path, true);
            draft_ = file_.settings;
            input_capture_.Cancel(); // what waited may be gone with the changes (a gamepad profile)
            LoadTexts();
            mscharged::platform::SetVerboseConsole(file_.settings.verbose_console);
            mscharged::platform::SetLogWindow(file_.settings.log_window);
            dirty_ = false;
            config_ok_ = true;
            // The pinned image's check stays while the image is the same (a
            // reverted library choice brings the old one back: check it).
            if (check_.selection != DiscSelection())
            {
                check_ = {};
                if (renderer_ && !disc_check_.Busy()) CheckDisc();
            }
            Notice(file_.exists ? texts::T("launcher.notices.settings_loaded")
                                : texts::T("launcher.notices.first_start"),
                   NoticeKind::Info);
        }
        catch (const std::exception& e)
        {
            LoadTexts();
            Notice(e.what(), NoticeKind::Error);
            config_ok_ = false;
        }
    }

    bool Save()
    {
        try
        {
            SaveConfig(file_, draft_);
            mscharged::platform::SetVerboseConsole(draft_.verbose_console);
            mscharged::platform::SetLogWindow(draft_.log_window);
            dirty_ = false;
            Notice(texts::T("launcher.notices.settings_saved"), NoticeKind::Success);
            return true;
        }
        catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); return false; }
    }

    // ------------------------------------------------------ server message
    // The profile server's message for the Play page (ServerMessageFetch),
    // asked once at the start. Development captures show a made-up one with
    // RECHARGED_CAPTURE_MESSAGE (its Markdown, "warning:" first for a
    // warning), or ask the server with RECHARGED_CAPTURE_MESSAGE=server.
    void StartServerMessage()
    {
        const char* preview = capture_ ? std::getenv("RECHARGED_CAPTURE_MESSAGE") : nullptr;
        if (capture_ && (!preview || std::string_view(preview) != "server"))
        {
            if (!preview || !*preview) return;
            ServerMessage message;
            message.id = 1;
            message.markdown = preview;
            if (message.markdown.rfind("warning:", 0) == 0)
            {
                message.warning = true;
                message.markdown.erase(0, 8);
            }
            ShowServerMessage(std::move(message));
            return;
        }
        if (!config_ok_ || !platform::HttpUnavailable().empty()) return;
        const auto server = platform::ProfileServerAddress(file_.settings.profile_server);
        if (!server) return;
        server_message_fetch_ = std::make_unique<ServerMessageFetch>(
            *server, [](const std::string& line) { platform::SessionLogLine(line); });
    }

    // Every frame: the answer, once it came.
    void PollServerMessage()
    {
        if (!server_message_fetch_) return;
        std::optional<ServerMessage> message;
        if (!server_message_fetch_->Done(&message)) return;
        server_message_fetch_.reset();
        if (message) ShowServerMessage(std::move(*message));
    }

    void ShowServerMessage(ServerMessage message)
    {
        server_message_blocks_ = ParseReleaseNotes(message.markdown);
        server_message_ = std::move(message);
    }

    // Whether the Play page shows it now: until it is over.
    bool ServerMessageShown() const
    {
        return server_message_ && ServerMessageCurrent(*server_message_, std::int64_t(std::time(nullptr)));
    }

    // The current path: a convex polygon with corners rounded to `radius`.
    static void PathRoundedPolygon(ImDrawList* draw, const ImVec2* corners, int count, float radius)
    {
        constexpr float pi = 3.14159265f;
        for (int i = 0; i < count; ++i)
        {
            const ImVec2 corner = corners[i], before = corners[(i + count - 1) % count],
                         after = corners[(i + 1) % count];
            const auto unit = [](ImVec2 v) {
                const float length = std::sqrt(v.x * v.x + v.y * v.y);
                return ImVec2{v.x / length, v.y / length};
            };
            const ImVec2 u = unit({before.x - corner.x, before.y - corner.y});
            const ImVec2 w = unit({after.x - corner.x, after.y - corner.y});
            const float angle = std::acos(std::clamp(u.x * w.x + u.y * w.y, -1.0f, 1.0f));
            const float along = radius / std::tan(angle * 0.5f), out = radius / std::sin(angle * 0.5f);
            const ImVec2 middle = unit({u.x + w.x, u.y + w.y});
            const ImVec2 centre{corner.x + middle.x * out, corner.y + middle.y * out};
            const float from = std::atan2(corner.y + u.y * along - centre.y, corner.x + u.x * along - centre.x);
            float turn = std::atan2(corner.y + w.y * along - centre.y, corner.x + w.x * along - centre.x) - from;
            while (turn > pi) turn -= 2 * pi;
            while (turn < -pi) turn += 2 * pi;
            draw->PathArcTo(centre, radius, from, from + turn, 10);
        }
    }

    // The message's sign, solid (as road signs are), its mark cut out in
    // `cut`: a rounded triangle with "!" for a warning, a disc with "i" for
    // information.
    static void DrawMessageSign(ImDrawList* draw, bool warning, ImVec2 c, float s, ImU32 ink, ImU32 cut)
    {
        const auto p = [&](float x, float y) { return ImVec2{c.x + x * s, c.y + y * s}; };
        const float bar = 0.06f * s;
        if (warning)
        {
            const ImVec2 corners[] = {p(0.0f, -0.47f), p(0.53f, 0.43f), p(-0.53f, 0.43f)};
            PathRoundedPolygon(draw, corners, 3, 0.11f * s);
            draw->PathFillConvex(ink);
            draw->AddRectFilled(p(-0.06f, -0.17f), p(0.06f, 0.13f), cut, bar);
            draw->AddCircleFilled(p(0.0f, 0.27f), 0.07f * s, cut, 16);
        }
        else
        {
            draw->AddCircleFilled(c, 0.48f * s, ink, 40);
            draw->AddCircleFilled(p(0.0f, -0.24f), 0.07f * s, cut, 16);
            draw->AddRectFilled(p(-0.06f, -0.08f), p(0.06f, 0.27f), cut, bar);
        }
    }

    // Across the Play page under its top line: the message on a light card,
    // its tone's colour and sign in a strip on its left, as tall as its text
    // up to a limit past which the text scrolls. Returns its height, which
    // the next frame's layout keeps room for (message_height_).
    float ServerMessageBanner(ImVec2 pos, float width)
    {
        const bool warning = server_message_->warning;
        const ImVec4 tint = warning ? color::warning : color::info;
        const ImVec4 ink{0.063f, 0.075f, 0.098f, 1.0f};
        const float strip = Dp(46), pad_x = Dp(18), pad_y = Dp(13), rounding = Dp(6);
        const float text_width = std::max(Dp(80), width - strip - pad_x * 2);
        // The text first, in a child as tall as it is (up to the limit); the
        // card is drawn after it into this window, which ImGui draws under
        // its children.
        ImGui::SetCursorScreenPos({pos.x + strip + pad_x, pos.y + pad_y});
        ImGui::SetNextWindowSizeConstraints({text_width, 0.0f}, {text_width, Dp(150)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
        ImGui::BeginChild("##message", {text_width, 0.0f}, ImGuiChildFlags_AutoResizeY);
        const float notes = ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0 ? Dp(10) : 0);
        DrawReleaseNotes(server_message_blocks_, notes, LightNotePalette());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        const float height = std::max(Dp(50), ImGui::GetItemRectSize().y + pad_y * 2);
        const ImVec2 max{pos.x + width, pos.y + height};
        auto* draw = ImGui::GetWindowDrawList();
        // Lifted off the artwork by a soft shadow.
        for (int i = 3; i >= 1; --i)
            draw->AddRectFilled({pos.x - Dp(float(i)), pos.y + Dp(float(i))},
                                {max.x + Dp(float(i)), max.y + Dp(2.0f * float(i))}, Col(color::window, 0.14f),
                                rounding + Dp(float(i)));
        draw->AddRectFilled(pos, max, Col(ImVec4{0.965f, 0.969f, 0.976f, 0.98f}), rounding);
        draw->AddRectFilled(pos, {pos.x + strip, max.y}, Col(tint), rounding, ImDrawFlags_RoundCornersLeft);
        DrawMessageSign(draw, warning, {std::floor(pos.x + strip * 0.5f), std::floor(pos.y + height * 0.5f)}, Dp(26),
                        Col(ink), Col(tint));
        message_height_ = height;
        return height;
    }

    // ------------------------------------------------------ players online
    // How many players are online on the WFC server (online.server), for the
    // Play page's top bar, asked every minute. Development captures stay
    // offline; RECHARGED_CAPTURE_ONLINE=<online>[/<in a match>] shows a
    // made-up count.
    void StartPlayersOnline()
    {
        if (capture_)
        {
            const char* preview = std::getenv("RECHARGED_CAPTURE_ONLINE");
            if (!preview || !*preview) return;
            char* end = nullptr;
            PlayersOnline::Count count;
            count.online = std::max<long long>(0, std::strtoll(preview, &end, 10));
            if (*end == '/') count.playing = std::clamp<long long>(std::strtoll(end + 1, nullptr, 10), 0, count.online);
            online_count_ = count;
            return;
        }
        if (!config_ok_ || file_.settings.online_server.empty()) return;
        players_online_ = std::make_unique<PlayersOnline>(
            file_.settings.online_server, [](const std::string& line) { platform::SessionLogLine(line); });
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
                // "problem-failed|cancelled|check|blocked": the box after an
                // enforced update that did not come in, as a release shows it.
                if (mode.rfind("problem-", 0) == 0)
                {
                    const std::string problem = mode.substr(8);
                    const UpdateProblem kind = problem == "failed" ? UpdateProblem::Failed
                        : problem == "cancelled"                   ? UpdateProblem::Cancelled
                        : problem == "check"                       ? UpdateProblem::CheckFailed
                        : problem == "blocked"                     ? UpdateProblem::Blocked
                                                                   : UpdateProblem::None;
                    update_preview_ = kind != UpdateProblem::None
                        && updates_->Preview(kind == UpdateProblem::Failed      ? "installfailed"
                                             : kind == UpdateProblem::CheckFailed ? "failed"
                                             : kind == UpdateProblem::Blocked     ? "blocked"
                                                                                  : "available");
                    if (update_preview_)
                    {
                        const auto preview = updates_->Snapshot();
                        ShowUpdateProblem(kind, kind == UpdateProblem::Blocked ? preview.install_blocker
                                                                               : preview.error);
                        update_problem_online_ = true;
                    }
                    return;
                }
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
        // Every start looks for a newer build: updates are enforced (StartEnforcedUpdate).
        if (config_ok_) CheckForUpdates(UpdatePrompt::Newer, UpdateChannelSetting());
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
        SaveNow([&](Settings& settings) {
            settings.update_channel = channel;
            settings.test_build = test_build;
        });
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
    // nothing changed. Test also goes back when there is no test build to go
    // to (or no network); Release and a menu choice with nothing to install
    // stay.
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

    // Enforced updates (owner 2026-11-12): releases and test builds install an
    // update of their own line by themselves, after the start's check and
    // after Check for updates; the dialog shows the download. A release that
    // stays behind plays offline: the game's main menu keeps Online closed
    // (launcher/online_gate.h). Local builds keep the choice.
    bool EnforcedUpdates() const { return updates_ && !update_preview_ && EnforcesUpdates(updates_->Build()); }

    // Why an enforced update did not come in (DrawUpdateProblem).
    enum class UpdateProblem { None, CheckFailed, Failed, Cancelled, Blocked };

    void StartEnforcedUpdate()
    {
        if (!update_.install_blocker.empty())
        {
            ShowUpdateProblem(UpdateProblem::Blocked, update_.install_blocker);
            return;
        }
        // A test build's pull request is followed from now on, also after it is released.
        if (update_.offer->test && draft_.test_build != update_.offer->pull_request)
            SaveUpdateChoice(draft_.update_channel, update_.offer->pull_request);
        updates_->Install();
        update_ = updates_->Snapshot();
        if (update_.state != UpdateService::State::Downloading) return;
        enforced_install_ = true;
        update_dialog_ = true;
    }

    // An installation the launcher started by itself: a failure or a
    // cancelled download opens the problem box; a finished one restarts.
    void FollowEnforcedInstall()
    {
        using State = UpdateService::State;
        if (!enforced_install_) return;
        if (update_.state == State::InstallFailed || update_.state == State::Available)
        {
            ShowUpdateProblem(update_.state == State::InstallFailed ? UpdateProblem::Failed : UpdateProblem::Cancelled,
                              update_.state == State::InstallFailed ? update_.error : std::string());
            // Nothing got installed: a switch that led here goes back (BeginSwitch).
            CancelSwitch();
        }
        else if (update_.state != State::Downloading && update_.state != State::Installing
                 && update_.state != State::Installed)
            enforced_install_ = false;
    }

    void ShowUpdateProblem(UpdateProblem problem, std::string reason)
    {
        enforced_install_ = false;
        update_problem_ = problem;
        update_problem_reason_ = std::move(reason);
        update_problem_open_ = true;
        update_problem_online_ = updates_ && GatesOnline(updates_->Build());
    }

    // When the update did not work out: what happened, that online play waits
    // for it (releases), and how it comes: Check for updates, or the next start.
    void DrawUpdateProblem()
    {
        if (update_problem_open_ && !ImGui::IsPopupOpen("Update") && !ImGui::IsPopupOpen("Welcome"))
        {
            ImGui::OpenPopup("Update problem");
            update_problem_open_ = false;
        }
        const auto& fonts = CurrentFonts();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(Dp(520), viewport->Size.x - Dp(48)), 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Update problem", nullptr, ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            const bool online = update_problem_online_;
            const char* heading = texts::T(
                update_problem_ == UpdateProblem::CheckFailed ? "launcher.update.problem.check_failed"
                : update_problem_ == UpdateProblem::Cancelled ? "launcher.update.problem.cancelled"
                : update_problem_ == UpdateProblem::Blocked   ? "launcher.update.problem.blocked"
                                                              : "launcher.update.problem.failed");
            TextColored(fonts.heading, color::text, heading);
            if (!update_problem_reason_.empty())
            {
                ImGui::Dummy({0, Dp(4)});
                TextWrappedColored(fonts.body, color::muted, update_problem_reason_.c_str());
            }
            ImGui::Dummy({0, Dp(12)});
            std::string text;
            if (online)
                text = std::string(texts::T(update_problem_ == UpdateProblem::CheckFailed
                                                ? "launcher.update.problem.online_off_check"
                                                : "launcher.update.problem.online_off_update"))
                    + " ";
            text += texts::T(update_problem_ == UpdateProblem::Blocked ? "launcher.update.problem.download_over"
                                                                       : "launcher.update.problem.try_later");
            if (online) Callout(text, color::warning, Icon::Warning);
            else TextWrappedColored(fonts.body, color::text, text.c_str());
            ImGui::Dummy({0, Dp(16)});
            const PackageAsset* asset = update_.offer && updates_ ? update_.offer->PackageFor(updates_->Build().package)
                                                                  : nullptr;
            const bool download = update_problem_ == UpdateProblem::Blocked && asset;
            const bool again = update_problem_ != UpdateProblem::Blocked;
            const char* action = download ? texts::T("launcher.update.dialog.download")
                : again                   ? texts::T("launcher.update.dialog.try_again")
                                          : nullptr;
            const float height = Dp(42);
            const float ok_width = Dp(100), action_width = action ? PrimaryWidth(action, Icon::None) : 0.0f;
            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            ImGui::SetCursorScreenPos({right - ok_width - (action ? Dp(10) + action_width : 0.0f),
                                       ImGui::GetCursorScreenPos().y});
            if (SecondaryButton("update_problem_ok", texts::T("launcher.update.problem.ok"), {ok_width, height}) || CancelPressed())
                ImGui::CloseCurrentPopup();
            if (action)
            {
                ImGui::SameLine(0, Dp(10));
                if (PrimaryButton("update_problem_action", action, {action_width, height}, true))
                {
                    ImGui::CloseCurrentPopup();
                    if (download) SDL_OpenURL(asset->url.c_str());
                    else CheckForUpdates(UpdatePrompt::Any, UpdateChannelSetting());
                }
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // When the launcher's last check found this build up to date (seconds
    // since 1970), for the game it starts (ResolvedLaunch::update_verified):
    // the release channel's answer only, which knows every newer release.
    std::int64_t UpdateVerified() const
    {
        if (!updates_ || update_preview_ || update_.channel != UpdateChannel::Release) return 0;
        return update_.state == UpdateService::State::UpToDate ? update_.checked : 0;
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

    // Blue for updates, amber for test builds, violet for the way back to the release.
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
        return update_.checked
            ? texts::T("launcher.update.checked_at", {{"time", LocalTime(update_.checked).substr(11)}})
            : std::string();
    }

    // On Test with no test build published (and this build no test build).
    bool NoTestBuilds() const
    {
        return update_.channel == UpdateChannel::Test && update_.test_builds.empty()
            && updates_->Build().origin != BuildOrigin::Test;
    }

    PillLook UpdatePillLook() const
    {
        using State = UpdateService::State;
        const bool test = update_.channel == UpdateChannel::Test;
        const std::string again = std::string("\n") + texts::T("launcher.update.pill.check_again");
        const std::string checked = update_.checked ? "\n" + LastChecked() : std::string();
        PillLook look;
        switch (update_.state)
        {
        case State::Idle:
            look = {Icon::Refresh, false, texts::T("launcher.update.pill.check"),
                    texts::T("launcher.update.pill.check_about"), color::muted};
            break;
        case State::Checking:
            look = {Icon::None, true, texts::T("launcher.update.pill.checking"), "", color::info};
            break;
        case State::UpToDate:
            if (ChooseTestBuild())
            {
                // Test builds to choose from: the click opens the Test menu.
                const auto count = update_.test_builds.size();
                look = {Icon::Flask, false,
                        count == 1 ? texts::T("launcher.update.pill.test_build_available")
                                   : texts::T("launcher.update.pill.choose_test_build"),
                        texts::Count("launcher.update.pill.test_builds_published", static_cast<long long>(count)) + "\n"
                            + texts::T("launcher.update.pill.choose_which"),
                        color::warning};
                look.filled = true;
                look.action = PillAction::Open;
                return look;
            }
            // Switched to Test while no test build is published: say why nothing changes.
            if (NoTestBuilds())
                look = {Icon::Check, false, texts::T("launcher.update.pill.no_test_builds"),
                        texts::T("launcher.update.pill.no_test_builds_about") + checked + again, color::warning};
            else
                look = {Icon::Check, false, texts::T("launcher.update.pill.up_to_date"),
                        (test ? texts::T("launcher.update.pill.no_newer_release_or_test")
                              : texts::T("launcher.update.pill.no_newer_release"))
                            + checked + again,
                        color::success};
            look.action = PillAction::Check;
            break;
        case State::CheckFailed:
            // Often just offline: shown quietly, the reason in the tooltip.
            look = {Icon::Warning, false, texts::T("launcher.update.pill.check_failed"), update_.error + again,
                    color::muted};
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
                look.text = update_.offer->test
                    ? std::string(texts::T("launcher.update.pill.new_test_build"))
                    : texts::T("launcher.update.pill.version_available",
                               {{"version", VersionNumber(update_.offer->version)}});
                look.tooltip = texts::T("launcher.update.pill.update_about");
                break;
            case OfferKind::TestBuild:
                look.text = texts::T("launcher.update.pill.test_build_available");
                look.tooltip = texts::T("launcher.update.pill.test_build_about");
                break;
            case OfferKind::BackToRelease:
                look.text = texts::T("launcher.update.pill.back_to_release",
                                     {{"version", VersionNumber(update_.offer->version)}});
                look.tooltip = texts::T("launcher.update.pill.back_to_release_about");
                break;
            }
            break;
        }
        case State::Downloading:
        {
            const auto percent = update_.total ? update_.received * 100 / update_.total : 0;
            look = {Icon::Download, false,
                    texts::T("launcher.update.pill.downloading", {{"percent", std::to_string(percent)}}),
                    texts::T("launcher.update.pill.downloading_about"), color::accent};
            look.progress = update_.total ? float(double(update_.received) / double(update_.total)) : 0.0f;
            break;
        }
        case State::Installing:
            look = {Icon::None, true, texts::T("launcher.update.installing"), "", color::info};
            break;
        case State::Installed:
            look = {Icon::None, true, texts::T("launcher.update.restarting"), "", color::accent};
            break;
        case State::InstallFailed:
            look = {Icon::Error, false, texts::T("launcher.update.pill.failed"), update_.error, color::danger};
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

    // The update status while it only offers a check (none yet, checking, up
    // to date, or the check failed): a round button with the refresh arrow,
    // the status in its tooltip. What a check found, a download and the
    // reason Test shows nothing stay a pill with their text.
    bool RoundUpdateButton(const PillLook& look) const
    {
        using State = UpdateService::State;
        const bool check = update_.state == State::Idle || update_.state == State::Checking
            || update_.state == State::UpToDate || update_.state == State::CheckFailed;
        return check && !look.filled && !(update_.state == State::UpToDate && NoTestBuilds());
    }

    bool DrawRefreshButton(const PillLook& look, float size)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##update_status", {size, size});
        const bool held = ImGui::IsItemActive();
        const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 12.0f);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 center{pos.x + size * 0.5f, pos.y + size * 0.5f};
        const float radius = size * 0.5f;
        // The same glass as the pill.
        draw->AddCircleFilled(center, radius,
                              Col(Mix(color::window, color::card_hover, (held ? 0.30f : 0.45f) + 0.55f * hover), 0.9f),
                              48);
        draw->AddCircle(center, radius - Dp(0.5f), Col(Mix(color::border, color::text, 0.15f * hover), 0.95f), 48,
                        Dp(1));
        const ImU32 ink = Col(Mix(color::muted, color::text, 0.65f + 0.35f * hover));
        if (look.spinner) Spinner(center, Dp(7), ink);
        else DrawIcon(draw, Icon::Refresh, center, Dp(17), ink);
        return pressed;
    }

    // "12 players online, 4 in a match"
    static std::string PlayersOnlineTooltip(const PlayersOnline::Count& count)
    {
        return texts::Count("launcher.play.online.about", count.online, {{"playing", std::to_string(count.playing)}});
    }

    // Top left of the Play page, while the WFC server tells: how many
    // players are online there (PlayersOnline). `left`: where it begins.
    void DrawPlayersOnline(float left, float y, float height)
    {
        if (!online_count_) return;
        const auto& fonts = CurrentFonts();
        const std::string text = texts::Count("launcher.play.online.count", online_count_->online);
        const ImVec2 label = TextSize(fonts.body, text.c_str());
        const float width = Dp(34) + label.x + Dp(18);
        const ImVec2 pos{left, y}, max{left + width, y + height};
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##players_online", {width, height});
        const bool hovered = ImGui::IsItemHovered();
        const float hover = Animate(ImGui::GetItemID(), hovered, 12.0f);
        auto* draw = ImGui::GetWindowDrawList();
        const float rounding = height * 0.5f, middle = y + height * 0.5f;
        const ImVec4 tint = online_count_->online > 0 ? color::success : color::muted;
        draw->AddRectFilled(pos, max, Col(Mix(color::window, color::card_hover, 0.45f + 0.55f * hover), 0.9f),
                            rounding);
        draw->AddRect(pos, max, Col(Mix(color::border, tint, 0.25f + 0.35f * hover), 0.95f), rounding, 0, Dp(1));
        // A live dot, breathing softly while anybody is online.
        const ImVec2 dot{pos.x + Dp(19), middle};
        if (online_count_->online > 0)
        {
            const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.4f);
            draw->AddCircleFilled(dot, Dp(4.5f + 3.0f * pulse), Col(tint, 0.22f * (1.0f - pulse)), 24);
        }
        draw->AddCircleFilled(dot, Dp(4.5f), Col(tint), 24);
        DrawLabel(draw, fonts.body, {pos.x + Dp(34), middle - label.y * 0.5f},
                  Col(Mix(color::muted, color::text, 0.65f + 0.35f * hover)), text.c_str());
        if (hovered) ImGui::SetTooltip("%s", PlayersOnlineTooltip(*online_count_).c_str());
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
        const bool switched = SegmentedIcons("##update_channel", &channel,
                                             {{texts::T("launcher.update.channel.release"), Icon::Tag, color::accent},
                                              {texts::T("launcher.update.channel.test"), Icon::Flask, color::warning,
                                               menu}},
                                             {switch_width, height}, true, &pressed);
        // Ctrl+click (Cmd+click on a Mac) on Test, a hidden switch: the test
        // builds their pull request hides (ShownBuilds) show until the
        // launcher closes, or hide again.
        const bool reveal = pressed == 1 && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper);
        if (reveal)
        {
            show_hidden_tests_ = !show_hidden_tests_;
            Notice(show_hidden_tests_ ? texts::T("launcher.update.channel.hidden_shown")
                                      : texts::T("launcher.update.channel.hidden_hidden"),
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
            ImGui::SetTooltip("%s", menu ? texts::T("launcher.update.channel.about_with_menu")
                                         : texts::T("launcher.update.channel.about"));
        const bool round = RoundUpdateButton(look);
        const float pill_width = round ? height : PillWidth(look);
        x -= Dp(10) + pill_width;
        ImGui::SetCursorScreenPos({x, y});
        if (round ? DrawRefreshButton(look, height) : DrawUpdatePill(look, {pill_width, height}))
        {
            if (ChooseTestBuild()) test_menu_ = true;
            else if (update_.state == State::Idle || update_.state == State::UpToDate
                     || update_.state == State::CheckFailed)
                CheckForUpdates(UpdatePrompt::Any, UpdateChannelSetting());
            else if (update_.offer)
                update_dialog_ = true;
        }
        if (ImGui::IsItemHovered())
        {
            const std::string tooltip
                = round ? look.text + (look.tooltip.empty() ? "" : "\n" + look.tooltip) : look.tooltip;
            if (!tooltip.empty()) ImGui::SetTooltip("%s", tooltip.c_str());
        }
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
        DrawTracked(draw, fonts.caption, {left + Dp(8), ImGui::GetCursorScreenPos().y}, Col(color::dim),
                    texts::T("launcher.update.test_menu.title"), Dp(1.2f));
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
            std::string detail =
                texts::T("launcher.update.test_build_number", {{"number", std::to_string(build.pull_request)}});
            if (build.published) detail += "  \xC2\xB7  " + LocalTime(build.published);
            if (build.hidden) detail += std::string("  \xC2\xB7  ") + texts::T("launcher.update.test_menu.hidden");
            const float line2 = pos.y + row_height - Dp(9) - FontSize(fonts.caption);
            DrawLabel(draw, fonts.caption, {text_left, line2}, Col(color::dim), detail.c_str());
            // Installed, or the running build's pull request has a newer build.
            if (running)
            {
                const char* tag = current ? texts::T("launcher.update.test_menu.installed")
                                          : texts::T("launcher.update.test_menu.update");
                const ImVec4 tint = current ? color::muted : color::accent;
                const float tag_x = text_left + TextSize(fonts.caption, detail.c_str()).x + Dp(10);
                DrawTracked(draw, fonts.caption, {tag_x, line2}, Col(tint), tag, Dp(0.8f));
            }
            if (followed) DrawIcon(draw, Icon::Check, {max.x - Dp(18), pos.y + row_height * 0.5f}, Dp(15), Col(color::warning));
            ImGui::Dummy({0, Dp(2)});
        }
        ImGui::Dummy({0, Dp(8)});
        TextWrappedColored(fonts.caption, color::dim, texts::T("launcher.update.test_menu.about"));
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
        case State::Idle: return texts::T("launcher.update.summary.not_checked");
        case State::Checking: return texts::T("launcher.update.summary.checking");
        case State::UpToDate:
            if (ChooseTestBuild()) return texts::T("launcher.update.summary.choose_test_build") + checked;
            return texts::T("launcher.update.summary.up_to_date") + checked;
        case State::CheckFailed: return texts::T("launcher.update.summary.check_failed", {{"error", update_.error}});
        case State::Available:
            switch (ClassifyOffer(updates_->Build(), *update_.offer))
            {
            case OfferKind::Update:
                return (update_.offer->test ? std::string(texts::T("launcher.update.summary.new_test_build"))
                                            : texts::T("launcher.update.summary.version_available",
                                                       {{"version", VersionNumber(update_.offer->version)}}))
                    + checked;
            case OfferKind::TestBuild: return texts::T("launcher.update.summary.test_build_available") + checked;
            case OfferKind::BackToRelease:
                return texts::T("launcher.update.summary.back_to_release",
                                {{"version", VersionNumber(update_.offer->version)}})
                    + checked;
            }
            break;
        case State::Downloading:
        case State::Installing:
        case State::Installed: return texts::T("launcher.update.summary.installing");
        case State::InstallFailed: return texts::T("launcher.update.summary.failed", {{"error", update_.error}});
        }
        return {};
    }

    // What the offered build changes, and its download and installation: a
    // card over the dimmed launcher. Where the launcher fills the screen
    // (Steam's Game Mode) the card keeps its size.
    void DrawUpdateDialog()
    {
        if (update_dialog_ && !ImGui::IsPopupOpen("Welcome"))
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
            if (update_.offer && !update_problem_open_) DrawUpdateDialogContents(*update_.offer);
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

    // A dialog's icon in a soft halo, `disc` wide.
    static void DialogBadge(ImDrawList* draw, ImVec2 center, float disc, Icon icon, const ImVec4& tint)
    {
        for (int i = 3; i >= 1; --i)
            draw->AddCircleFilled(center, disc * 0.5f + Dp(6.0f * float(i)), Col(tint, 0.025f));
        draw->AddCircleFilled(center, disc * 0.5f, Col(Mix(DialogFill(), tint, 0.16f)));
        DrawIcon(draw, icon, center, Dp(21), Col(tint));
    }

    // Notes in a dialog, at most `height` tall: no frame, the right stick
    // scrolls them and the text fades out where it scrolls.
    static void DrawNotesPane(const std::vector<NoteBlock>& notes, float height, const char* empty)
    {
        ImGui::SetNextWindowSizeConstraints({0, Dp(24)}, {FLT_MAX, height});
        ImGui::BeginChild("##notes", {ImGui::GetContentRegionAvail().x, 0}, ImGuiChildFlags_AutoResizeY);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Dp(10), 0.0f});
            const float text_width = ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0 ? Dp(10) : 0);
            if (notes.empty()) TextWrappedColored(CurrentFonts().body, color::muted, empty);
            else DrawReleaseNotes(notes, text_width);
            ImGui::PopStyleVar();
            const float stick = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue
                - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
            if (std::fabs(stick) > 0.15f)
                ImGui::SetScrollY(ImGui::GetScrollY() + stick * Dp(700) * ImGui::GetIO().DeltaTime);
            const float scroll = ImGui::GetScrollY(), scroll_max = ImGui::GetScrollMaxY();
            if (scroll_max > 0.0f)
            {
                auto* draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                const float fade = Dp(30), right = at.x + size.x - ImGui::GetStyle().ScrollbarSize;
                const ImU32 solid = Col(DialogFill()), clear = Col(DialogFill(), 0.0f);
                if (scroll < scroll_max - 0.5f)
                    draw->AddRectFilledMultiColor({at.x, at.y + size.y - fade}, {right, at.y + size.y}, clear, clear,
                                                  solid, solid);
                if (scroll > 0.5f) draw->AddRectFilledMultiColor(at, {right, at.y + fade}, solid, solid, clear, clear);
            }
        }
        ImGui::EndChild();
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

    // A small framed action button's width (SecondaryButton: icon, gap, label).
    static float ActionWidth(const char* label, Icon icon)
    {
        return TextSize(CurrentFonts().body, label).x + (icon == Icon::None ? 0.0f : Dp(18) + Dp(8)) + Dp(28);
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
        const std::string pull_request_line =
            texts::T("launcher.update.dialog.pull_request", {{"number", std::to_string(offer.pull_request)}});
        const std::string version_line =
            texts::T("launcher.update.dialog.version", {{"version", VersionNumber(offer.version)}});
        switch (kind)
        {
        case OfferKind::Update:
            overline = offer.test ? texts::T("launcher.update.dialog.new_test_build")
                                  : texts::T("launcher.update.dialog.update_available");
            headline = offer.test ? pull_request_line : version_line;
            break;
        case OfferKind::TestBuild:
            overline = texts::T("launcher.update.dialog.install_test_build");
            headline = pull_request_line;
            break;
        case OfferKind::BackToRelease:
            overline = texts::T("launcher.update.dialog.install_release");
            headline = version_line;
            break;
        }
        const std::string title = offer.test ? (status.pull && !status.pull->title.empty() ? status.pull->title
                                                                                           : offer.title)
                                             : ReleaseSubtitle(offer.title, offer.version);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float disc = Dp(46);
        const ImVec2 center{top.x + disc * 0.5f, top.y + Dp(2) + disc * 0.5f};
        DialogBadge(draw, center, disc, OfferIcon(kind, offer), tint);
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
        const auto test_build = [](int number) {
            return texts::T("launcher.update.test_build_number", {{"number", std::to_string(number)}});
        };
        const std::string mine_what = self.origin == BuildOrigin::Release ? texts::T("launcher.update.dialog.release")
            : self.origin == BuildOrigin::Test ? test_build(self.pull_request)
                                               : texts::T("launcher.update.dialog.local_build");
        const std::string from = describe(mine, mine_what);
        const std::string titled = offer.test ? TitleVersion(offer) : std::string();
        const std::string to = !titled.empty()
            ? titled + "  \xC2\xB7  " + test_build(offer.pull_request)
            : describe(offer.version,
                       offer.test ? test_build(offer.pull_request) : texts::T("launcher.update.dialog.release"));
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
                meaning = texts::T("launcher.update.dialog.replaces_test_build",
                                   {{"from", std::to_string(self.pull_request)},
                                    {"to", std::to_string(offer.pull_request)}});
            else if (kind == OfferKind::TestBuild)
                meaning = texts::T("launcher.update.dialog.about_test_builds");
            else
                meaning = texts::T("launcher.update.dialog.replaces_with_release");
            if (kind == OfferKind::BackToRelease && !UpdatesItself(offer.version))
                meaning += " "
                    + texts::T("launcher.update.dialog.cannot_update_itself",
                               {{"version", VersionNumber(offer.version)}});
            Callout(meaning, tint, kind == OfferKind::TestBuild ? Icon::Warning : Icon::Info);
            ImGui::Dummy({0, Dp(20)});
            callout = true;
        }

        // The release notes, or the test build's pull request description:
        // no frame, the text fades out where it scrolls.
        DrawTracked(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(color::dim),
                    offer.test ? texts::T("launcher.update.dialog.pull_request_notes")
                               : texts::T("launcher.update.dialog.release_notes"),
                    Dp(1.2f));
        ImGui::Dummy({0, caption + Dp(10)});
        const std::string& source = offer.test && status.pull ? status.pull->body : offer.notes;
        if (source != notes_source_)
        {
            notes_source_ = source;
            notes_ = WithoutNotesFooter(ParseReleaseNotes(source));
        }
        float notes_height = ImGui::GetMainViewport()->Size.y * 0.36f;
        if (callout) notes_height -= Dp(64);
        if (status.state == State::InstallFailed || !status.install_blocker.empty()) notes_height -= Dp(56);
        DrawNotesPane(notes_, std::clamp(notes_height, Dp(90), Dp(340)), texts::T("launcher.update.dialog.no_notes"));
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
            const std::string progress = status.total
                ? texts::T("launcher.update.dialog.downloading_of",
                           {{"received", Megabytes(status.received)}, {"total", Megabytes(status.total)}})
                : texts::T("launcher.update.dialog.downloading", {{"received", Megabytes(status.received)}});
            const float line = ImGui::GetCursorScreenPos().y;
            TextColored(fonts.caption, color::muted, progress.c_str());
            if (status.total)
            {
                const std::string done = std::to_string(status.received * 100 / status.total);
                const std::string percent = texts::T("launcher.update.dialog.percent", {{"percent", done}});
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
            Callout(texts::T("launcher.update.dialog.download_instead", {{"reason", status.install_blocker}}),
                    color::muted, Icon::Info);
            ImGui::Dummy({0, Dp(18)});
        }
        const bool ask_to_save = status.state == State::Installed && dirty_;
        if (ask_to_save)
        {
            // The launcher restarts into the new version; settings first.
            TextWrappedColored(fonts.body, color::text, texts::T("launcher.update.dialog.save_before_restart"));
            ImGui::Dummy({0, Dp(18)});
        }

        // Footer: the build's page on GitHub on the left, the actions on the right.
        DialogDivider();
        ImGui::Dummy({0, Dp(18)});
        const float row = ImGui::GetCursorScreenPos().y, height = Dp(40);
        auto align_right = [&](float width) { ImGui::SetCursorScreenPos({right_edge - width, row}); };
        if (ask_to_save)
        {
            const char *discard = texts::T("launcher.update.dialog.discard_restart"),
                       *save = texts::T("launcher.update.dialog.save_restart");
            align_right(GhostWidth(discard, Icon::None) + Dp(10) + PrimaryWidth(save, Icon::None));
            if (GhostButton("update_discard_restart", discard, {GhostWidth(discard, Icon::None), height})) Reload();
            ImGui::SameLine(0, Dp(10));
            if (PrimaryButton("update_save_restart", save, {PrimaryWidth(save, Icon::None), height}, true)) Save();
            ImGui::SetItemDefaultFocus();
        }
        else if (status.state == State::Downloading)
        {
            const char* cancel = texts::T("launcher.update.dialog.cancel_download");
            align_right(GhostWidth(cancel, Icon::None));
            if (GhostButton("update_cancel", cancel, {GhostWidth(cancel, Icon::None), height})) updates_->Cancel();
        }
        else if (busy)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            Spinner({pos.x + Dp(8), pos.y + height * 0.5f}, Dp(7), Col(tint));
            DrawLabel(draw, fonts.body, {pos.x + Dp(26), pos.y + (height - FontSize(fonts.body)) * 0.5f},
                      Col(color::text),
                      status.state == State::Installing ? texts::T("launcher.update.installing")
                                                        : texts::T("launcher.update.restarting"));
            ImGui::Dummy({1.0f, height});
        }
        else
        {
            const std::string page = offer.test && status.pull && !status.pull->page.empty() ? status.pull->page
                                                                                             : offer.page;
            const char* github = texts::T("launcher.update.dialog.view_on_github");
            if (!page.empty())
            {
                if (GhostButton("update_github", github, {GhostWidth(github, Icon::GitHub), height}, true,
                                Icon::GitHub))
                    SDL_OpenURL(page.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", page.c_str());
            }
            const char* action = nullptr;
            if (status.install_blocker.empty())
                action = status.state == State::InstallFailed ? texts::T("launcher.update.dialog.try_again")
                    : kind == OfferKind::Update               ? texts::T("launcher.update.dialog.update_now")
                                                              : texts::T("launcher.update.dialog.switch_now");
            else if (asset) action = texts::T("launcher.update.dialog.download");
            const char* later = texts::T("launcher.update.dialog.later");
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
                {
                    const std::string package = asset ? asset->name : texts::T("launcher.update.dialog.the_package");
                    const std::string tip = texts::T("launcher.update.dialog.update_now_about", {{"package", package}});
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::SetItemDefaultFocus();
            }
        }
    }

    // ------------------------------------------------------------- welcome
    // The welcome guide, assets/launcher/welcome.md: its first "#" heading is
    // the dialog's title, the rest its text. Without the file there is no guide.
    void LoadWelcome(const fs::path& path)
    {
        std::size_t size = 0;
        void* data = SDL_LoadFile(PathUtf8(path).c_str(), &size);
        if (!data) return;
        welcome_ = ParseReleaseNotes({static_cast<const char*>(data), size});
        SDL_free(data);
        if (!welcome_.empty() && welcome_.front().kind == NoteBlock::Kind::Heading && welcome_.front().level == 1)
        {
            for (const auto& span : welcome_.front().spans) welcome_title_ += span.text;
            welcome_.erase(welcome_.begin());
        }
    }

    // The guide as a card over the dimmed launcher, like the update dialog. It
    // opens when no other dialog or menu is open (an update found at the start
    // waits for it), and opens again after one that took its place.
    void DrawWelcome()
    {
        if (welcome_pending_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::OpenPopup("Welcome");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(Dp(640), viewport->Size.x - Dp(48));
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({width, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(30, 26));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0, 0, 0, 0});
        if (ImGui::BeginPopupModal("Welcome", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Dp(10), 0.0f});
            DrawWelcomeContents();
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void DrawWelcomeContents()
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec4 tint = color::accent;
        DrawDialogCard(tint);
        const float right_edge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;

        // Header: the badge, then the guide's title.
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float disc = Dp(46);
        DialogBadge(draw, {top.x + disc * 0.5f, top.y + Dp(2) + disc * 0.5f}, disc, Icon::Bulb, tint);
        ImGui::SetCursorScreenPos({top.x + disc + Dp(20), top.y});
        ImGui::BeginGroup();
        DrawTracked(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(tint, 0.95f),
                    texts::T("launcher.welcome.overline"), Dp(1.4f));
        ImGui::Dummy({0, FontSize(fonts.caption) + Dp(5)});
        // Without a heading in the guide: the launcher's own title.
        TextColored(fonts.heading, color::text,
                    welcome_title_.empty() ? texts::T("launcher.welcome.title") : welcome_title_.c_str());
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos({top.x, std::max(top.y + disc + Dp(4), ImGui::GetItemRectMax().y)});
        ImGui::Dummy({0, Dp(20)});

        DrawNotesPane(welcome_, std::clamp(ImGui::GetMainViewport()->Size.y * 0.5f, Dp(120), Dp(440)), "");
        ImGui::Dummy({0, Dp(18)});

        DialogDivider();
        ImGui::Dummy({0, Dp(18)});
        const char* start = texts::T("launcher.welcome.get_started");
        const float start_width = PrimaryWidth(start, Icon::Check);
        ImGui::SetCursorScreenPos({right_edge - start_width, ImGui::GetCursorScreenPos().y});
        if (PrimaryButton("welcome_start", start, {start_width, Dp(40)}, true, Icon::Check) || CancelPressed())
            CloseWelcome();
        ImGui::SetItemDefaultFocus();
    }

    // Closed, the guide no longer opens by itself: saved at once, without the
    // other changes not saved yet (as the update choice is).
    void CloseWelcome()
    {
        ImGui::CloseCurrentPopup();
        welcome_pending_ = false;
        if (file_.settings.show_welcome) SaveNow([](Settings& settings) { settings.show_welcome = false; });
        else draft_.show_welcome = false;
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
            Notice(texts::T("launcher.image.command_line"), NoticeKind::Info);
            return;
        }
        candidate_.Start([path, known = KnownBanners()] { return RunDiscCheck(PathFromUtf8(path), path, known); });
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
            Notice(texts::T("launcher.image.unplayable", {{"name", name}, {"reason", UnplayableReason(*result.info)}}),
                   NoticeKind::Warning);
        }
        else Notice(texts::T("launcher.image.unusable", {{"name", name}, {"error", result.error}}), NoticeKind::Error);
    }

    // Makes this playable image the one Play starts. Only an explicit choice
    // pins an image: scans, sorting and searches never do.
    void Pin(const fs::path& path, const DiscInfo& info, std::uintmax_t size = 0)
    {
        if (options_.launch.disc)
        {
            Notice(texts::T("launcher.image.command_line"), NoticeKind::Info);
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
                disc_check_.Drop();
                check_ = {};
                return;
            }
            const auto selection = PathUtf8(path);
            check_ = {};
            disc_check_.Start([path, selection, known = KnownBanners()] { return RunDiscCheck(path, selection, known); });
        }
        catch (const std::exception& e)
        {
            disc_check_.Drop();
            check_.error = e.what();
        }
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
        if (search_.Busy()) return;
        std::error_code error;
        const auto user_folder = [](SDL_Folder which) {
            const char* path = SDL_GetUserFolder(which);
            return path ? PathFromUtf8(path) : fs::path();
        };
        const auto settings = fs::absolute(file_.path, error).parent_path();
        const auto home = user_folder(SDL_FOLDER_HOME), downloads = user_folder(SDL_FOLDER_DOWNLOADS);
        search_from_ = draft_.disc;
        search_folder_from_ = draft_.folder;
        search_.Start([settings, home, downloads] {
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
        // (Not when the player chose a game folder meanwhile: its scan decides.)
        if (!found.empty() && !folder_has_game && draft_.folder == search_folder_from_ && !library_scan_.Busy())
        {
            draft_.folder = SettingsPath(found.parent_path());
            dirty_ = true;
            moved = std::string(" ") + texts::T("launcher.image.folder_moved");
        }
        if (result.disc.empty())
        {
            if (!result.other.empty())
            {
                const DiscInfo other{"", result.other_id, "", result.other_revision, 0};
                Notice(texts::T("launcher.image.found_other", {{"path", PathUtf8(result.other)},
                                                               {"reason", UnplayableReason(other)},
                                                               {"releases", PlayableReleasesText()}})
                           + moved,
                       NoticeKind::Warning);
            }
            if (clean && !moved.empty()) Save();
            return;
        }
        draft_.disc = SettingsPath(result.disc);
        dirty_ = true;
        check_ = {};
        if (!disc_check_.Busy()) CheckDisc();
        std::string text = texts::T("launcher.image.found", {{"name", PathUtf8(result.disc.filename())}}) + moved;
        if (result.supported > 1) text += std::string(" ") + texts::T("launcher.image.found_more");
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
        Notice(texts::T("launcher.image.game_folder", {{"path", PathUtf8(folder)}}), NoticeKind::Info);
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
        library_strikers_ = {};
        if (library_scan_.Busy())
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
        library_scan_folders_ = folders;
        library_scan_.Start([folders, cache = image_cache_, known = KnownBanners(), keep, progress = library_progress_] {
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
                Notice(texts::T("launcher.image.chosen_from_folder", {{"name", PathUtf8(image->path.filename())},
                                                                      {"release", RegionRevision(image->probe.info)}}),
                       NoticeKind::Success);
                return;
            }
        if (reason == AutoChoice::FirstStart) SearchUsualPlaces();
        else
            Notice(texts::T("launcher.image.none_in_folder"), NoticeKind::Warning);
    }

    // Once a frame: scan when the game folder changes or when it was
    // not available comes back, and notice when a missing pinned image comes
    // back (a drive plugged in again).
    void SyncLibrary()
    {
        if (!config_ok_) return;
        if (draft_.folder != scanned_folder_ || draft_.folder_subfolders != scanned_subfolders_) StartLibraryScan();
        if (!library_scan_.Busy() && SDL_GetTicks() - folders_probe_ > 3000)
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
        if (!disc_check_.Busy() && !check_.missing.empty() && check_.selection == DiscSelection()
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
        // The dialog may read them until it closes, so they stay.
        static SDL_DialogFileFilter filters[] = {{"", "iso;rvz;wbfs;ISO;RVZ;WBFS"}, {"", "*"}};
        filters[0].name = texts::T("launcher.image.filter_disc_images");
        filters[1].name = texts::T("launcher.image.filter_all_files");
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
        if (auto cards = graphics_cards_scan_.Take())
        {
            graphics_cards_ = std::move(*cards);
            platform::SessionLogLine(platform::DescribeGraphicsCards(graphics_cards_));
        }
        UpdateCompetitive();
        if (players_online_) online_count_ = players_online_->Snapshot();
        PollServerMessage();
        if (updates_)
        {
            update_ = updates_->Snapshot();
            FollowEnforcedInstall();
            if (update_prompt_ != UpdatePrompt::None && update_.state != UpdateService::State::Checking)
            {
                // After the hidden switch: the menu, with whatever test builds there are.
                if (update_prompt_ == UpdatePrompt::Reveal)
                {
                    if (!update_.test_builds.empty()) test_menu_ = true;
                }
                else if (update_.state == UpdateService::State::Available && update_.offer)
                {
                    // An update of the own line installs by itself (enforced
                    // updates); after the start the dialog offers only newer
                    // builds: an update or a newer test build.
                    const OfferKind kind = ClassifyOffer(updates_->Build(), *update_.offer);
                    if (kind == OfferKind::Update && EnforcedUpdates()) StartEnforcedUpdate();
                    else if (update_prompt_ != UpdatePrompt::Newer || kind == OfferKind::Update
                        || (kind == OfferKind::TestBuild && IsNewer(updates_->Build(), *update_.offer)))
                        update_dialog_ = true;
                }
                // A release that cannot check plays offline: say so.
                else if (update_.state == UpdateService::State::CheckFailed && EnforcedUpdates()
                         && GatesOnline(updates_->Build()))
                    ShowUpdateProblem(UpdateProblem::CheckFailed, update_.error);
                else if (update_prompt_ == UpdatePrompt::Switch && ChooseTestBuild())
                    test_menu_ = true;
                // Test without a test build to go to (none for this system, or the
                // check failed): the switch goes back to what it was.
                else if (update_prompt_ == UpdatePrompt::Switch
                         && (update_.state == UpdateService::State::CheckFailed || update_.test_builds.empty()))
                {
                    Notice(update_.state == UpdateService::State::CheckFailed
                               ? texts::T("launcher.update.channel.check_failed", {{"error", update_.error}})
                               : std::string(texts::T("launcher.update.channel.no_test_build")),
                           update_.state == UpdateService::State::CheckFailed ? NoticeKind::Warning : NoticeKind::Info);
                    CancelSwitch();
                }
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
        if (auto taken = disc_check_.Take())
        {
            DiscCheck result = std::move(*taken);
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
        if (auto result = candidate_.Take()) CandidateChecked(std::move(*result));
        // Texture packs: read while the Extras page is open; a dropped pack's
        // result also arrives on another page.
        for (const auto& event : pack_library_.Poll(TextureFolder(), page_ == PageExtras && config_ok_))
            TexturePackEvent(event);
        if (auto scan = library_scan_.Take()) LibraryScanDone(std::move(*scan));
        if (auto found = search_.Take()) DiscSearchDone(std::move(*found));
    }

    // Graphics -> Graphics card (display.gpu): Automatic or one of the PC's cards.
    void GraphicsCardRow()
    {
#if defined(_WIN32)
        Row(texts::T("launcher.graphics_card.label"), texts::T("launcher.graphics_card.about_windows"));
#else
        Row(texts::T("launcher.graphics_card.label"), texts::T("launcher.graphics_card.about"));
#endif
        std::optional<GraphicsCardId> chosen;
        try { chosen = ParseGraphicsCard(draft_.gpu); }
        catch (const std::invalid_argument&) {}
        const char* automatic = texts::T("launcher.graphics_card.automatic");
        std::string preview = automatic;
        if (chosen)
        {
            preview = graphics_cards_scan_.Busy() ? draft_.gpu
                                              : texts::T("launcher.graphics_card.not_found", {{"card", draft_.gpu}});
            for (const auto& card : graphics_cards_)
                if (card.id == *chosen) preview = card.name;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##graphics_card", preview.c_str()))
        {
            if (ImGui::Selectable((std::string(automatic) + "###auto").c_str(), !chosen))
            { draft_.gpu = "auto"; dirty_ = true; }
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
            if (graphics_cards_scan_.Busy()) ImGui::TextDisabled("%s", texts::T("launcher.graphics_card.looking"));
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
            gamepad_names_.push_back(name ? name : texts::T("launcher.devices.unnamed_controller"));
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
            const char* shown = name && *name ? name : texts::T("launcher.devices.unnamed_display");
            std::string label = std::to_string(i + 1) + "  " + shown;
            if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]))
                label += "  (" + std::to_string(mode->w) + " x " + std::to_string(mode->h) + ")";
            if (ids[i] == primary) label += std::string("  - ") + texts::T("launcher.devices.primary_display");
            displays_.push_back(std::move(label));
            display_ids_.push_back(ids[i]);
        }
        SDL_free(ids);
    }

    bool OpenPath(const fs::path& path)
    {
        if (!SDL_OpenURL(FileUrl(path).c_str()))
        {
            Notice(texts::T("launcher.notices.cannot_open_folder", {{"error", SDL_GetError()}}), NoticeKind::Error);
            return false;
        }
        return true;
    }

    // What keeps this computer from a set: whatever keeps it from playing,
    // and an update it lacks (enforced updates, launcher/online_gate.h): an
    // official release whose check failed, or with a newer release it has not
    // installed, finds Online closed in its game. Empty when a set can start.
    std::string SetBlocker() const
    {
        if (std::string blocker = PlayBlocker(); !blocker.empty()) return blocker;
        if (!updates_ || update_preview_ || !GatesOnline(updates_->Build())
            || update_.channel != UpdateChannel::Release)
            return {};
        using State = UpdateService::State;
        if (update_.state == State::CheckFailed) return texts::T("launcher.challenge.update_unchecked");
        if (update_.state == State::Available || update_.state == State::InstallFailed)
            return texts::T("launcher.challenge.update_outdated");
        return {};
    }

    // Why Play is unavailable, or empty when the game can start.
    std::string PlayBlocker() const
    {
#ifndef MSCHARGED_HAS_ORIGINAL_FRONTEND
        return texts::T("launcher.play.blocker.no_game");
#else
        // The installed files belong to the next launcher: never start them from this one.
        if (update_.state == UpdateService::State::Installing || update_.state == UpdateService::State::Installed)
            return texts::T("launcher.play.blocker.updating");
        // Enforced updates: the check comes first, and an update it found.
        if (EnforcedUpdates() && update_.state == UpdateService::State::Checking)
            return texts::T("launcher.play.blocker.checking_updates");
        if (enforced_install_) return texts::T("launcher.play.blocker.installing_update");
        if (!config_ok_) return texts::T("launcher.play.blocker.settings_problem");
        if (search_.Busy()) return texts::T("launcher.play.blocker.searching");
        if (disc_check_.Busy()) return texts::T("launcher.play.blocker.checking");
        if (DiscSelection().empty())
            return autochoose_ != AutoChoice::None && library_scan_.Busy()
                ? texts::T("launcher.play.blocker.reading_folder")
                : texts::T("launcher.play.blocker.choose_image");
        if (!PinnedMissing().empty())
            return texts::T("launcher.play.blocker.image_missing", {{"reason", PinnedMissing()}});
        if (!check_.info || check_.selection != DiscSelection())
            return check_.error.empty() ? std::string(texts::T("launcher.play.blocker.check_image")) : check_.error;
        if (!SupportedRelease(*check_.info)) return UnplayableReason(*check_.info);
        const auto settings = EffectiveLaunch().settings;
        try { ResolveGraphicsBackend(settings.backend); }
        catch (const std::invalid_argument& error) { return error.what(); }
        // The values the game takes (an older settings file's auto too).
        if (settings.aspect != "auto" && settings.aspect != "4:3" && settings.aspect != "16:9"
            && settings.aspect != "stretch")
            return texts::T("launcher.play.blocker.aspect");
        // A language this release does not have starts as Automatic (the Game page says so).
        return {};
#endif
    }

    // Online needs the game image: the game plays online, and the image's Mii
    // parts draw the Miis. While the chosen image is still being checked it
    // counts as there.
    std::string OnlineBlocker() const
    {
        // Development captures and smoke tests of the made-up online states
        // (RECHARGED_CAPTURE_DISCORD=preview...) show the pages without one.
        if (const char* preview = capture_ ? std::getenv("RECHARGED_CAPTURE_DISCORD") : nullptr;
            preview && std::string(preview).rfind("preview", 0) == 0)
            return {};
        if (DiscSelection().empty())
            return texts::T("launcher.online_blocker.choose_image");
        if (check_.selection == DiscSelection())
        {
            if (!check_.info && !check_.error.empty())
                return texts::T("launcher.online_blocker.unreadable", {{"error", check_.error}});
            if (check_.info && !SupportedRelease(*check_.info)) return UnplayableReason(*check_.info);
        }
        return {};
    }

    void Play(bool& quit, bool training = false)
    {
        if (!PlayBlocker().empty()) return;
        training_ = training;
        // Unsaved changes: ask whether to start with or without them.
        if (dirty_) { play_prompt_ = true; return; }
        Start(quit);
    }

    // The game's launch from the launcher's choices, for Play and a set's go.
    ResolvedLaunch GameLaunch() const
    {
        ResolvedLaunch launch = EffectiveLaunch();
        launch.update_verified = UpdateVerified();
        launch.discord_friends = discord_asked_;
        return launch;
    }

    void Start(bool& quit)
    {
        frontend_launch_ = GameLaunch();
        frontend_launch_->start_training = training_;
        // A replay starts with the image it found; the game closes after it.
        if (ReplayReady())
        {
            frontend_launch_->start_training = false;
            frontend_launch_->disc_path = pending_replay_->image;
            frontend_launch_->replay_path = pending_replay_->path;
            // A kept game of a match: the game follows the match's next one.
            if (pending_replay_->path == kept_file_) frontend_launch_->replay_next_link = kept_next_link_;
            kept_file_.clear();
            kept_next_link_.clear();
            platform::SessionLogLine("Replay: starting with " + PathUtf8(pending_replay_->image));
            pending_replay_.reset();
            quit = true;
            return;
        }
        // A set that goes while the game runs uses the player's own Mii: the
        // chosen one, or the one made from the account (made now if it isn't
        // there yet). Without it the game lets the player choose a Mii for
        // the set, so Play goes on.
        if (ChosenOwnMii())
        {
            if (const auto own = PlayerOwnMii(); own && own->here) frontend_launch_->competitive_mii = own->id;
        }
        else if (const auto key = PlayerMiiKey(); !key.empty())
        {
            try
            {
                EnsurePlayerMii();
                frontend_launch_->competitive_mii = mscharged::platform::KeyedMiiCreateId(key);
            }
            catch (const std::exception& e)
            {
                platform::SessionLogLine(std::string("Your Mii for online sets could not be made: ") + e.what());
                Notice(texts::T("launcher.notices.set_mii_failed", {{"error", e.what()}}), NoticeKind::Warning);
            }
        }
        quit = true;
    }

    // A replay to play (--replay, a .msc file dropped on the window, or
    // the Replays page). It needs an image of the release and revision it was
    // recorded with: the chosen image when it is one, otherwise one of the
    // game folder; the game then starts with it, without changing the chosen
    // image. Unsaved changes are asked about first, as Play does.
    struct PendingReplay
    {
        fs::path path;
        DiscInfo needs;
        fs::path image; // once found
        bool any_revision = false; // a watch ticket: any image of the release
    };
    std::optional<PendingReplay> pending_replay_;
    // A game the WFC server kept (a ticket naming it, from the dashboard's
    // Replay button): its file comes from the server first, then it plays as
    // any replay; next_link is the match's next kept game, for the game to
    // follow once the replay ended.
    struct KeptDownload
    {
        fs::path file; // empty when it failed
        std::string next_link;
        std::string error;
    };
    BackgroundJob<KeptDownload> kept_download_;
    fs::path kept_file_;
    std::string kept_next_link_;

    void FetchKept(const replay::WatchTicket& ticket)
    {
        const std::string server = file_.settings.online_server;
        if (server.empty())
        {
            Notice(texts::T("launcher.replay.unreadable",
                            {{"error", "Watching a match needs the online server that made the ticket: set it "
                                       "in the launcher's Online page."}}),
                   NoticeKind::Error);
            return;
        }
        const std::string name =
            "game " + std::to_string(ticket.game) + " of match " + std::to_string(ticket.match);
        platform::SessionLogLine("Watch ticket: " + name + " (" + ticket.release + ") from " + server);
        Notice(texts::T("launcher.replay.starting", {{"name", name}}), NoticeKind::Info);
        kept_download_.Start([server, ticket] {
            KeptDownload result;
            std::vector<std::uint8_t> bytes;
            int next = 0;
            if (!platform::FetchKeptReplay(server, ticket, bytes, next, result.error)) return result;
            const fs::path folder = fs::temp_directory_path() / "recharged-watch";
            const fs::path target = folder
                / ("match-" + std::to_string(ticket.match) + "-game-" + std::to_string(ticket.game) + ".msc");
            std::error_code failure;
            fs::create_directories(folder, failure);
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();
            if (!out)
            {
                result.error = "The replay could not be written to " + PathUtf8(folder) + ".";
                return result;
            }
            result.file = target;
            if (next > 0)
                result.next_link = "recharged://replay/" + std::to_string(ticket.match) + "?game="
                    + std::to_string(next) + "&ticket=" + ticket.ticket + "&release=" + ticket.release;
            return result;
        });
    }

    // The kept game's file, once it came: it plays as any replay.
    void PollKept()
    {
        auto kept = kept_download_.Take();
        if (!kept) return;
        if (kept->file.empty())
        {
            platform::SessionLogLine("Watch ticket: " + kept->error);
            Notice(texts::T("launcher.replay.unreadable", {{"error", kept->error}}), NoticeKind::Error);
            return;
        }
        kept_file_ = kept->file;
        kept_next_link_ = kept->next_link;
        AskReplay(kept->file);
    }

    bool ReplayReady() const { return pending_replay_ && !pending_replay_->image.empty(); }

    void AskReplay(const fs::path& path)
    {
        if (pending_replay_) return;
        // The whole file is checked here: a damaged one does not start the game.
        std::vector<std::uint8_t> bytes, stream;
        std::string error;
        replay::FileInfo info;
        replay::Setup setup;
        // A watch ticket from the WFC server's dashboard: the game watches the
        // match live, with any image of its release.
        replay::WatchTicket ticket;
        if (replay::ReadReplayFile(path, bytes, error) && replay::ReadWatchTicket(bytes, ticket, error))
        {
            // A game the server kept: its file first, whose release and
            // revision then choose the image.
            if (ticket.game > 0)
            {
                FetchKept(ticket);
                return;
            }
            DiscInfo needs{};
            needs.game_id = ticket.release;
            pending_replay_ = PendingReplay{path, needs, {}, true};
            platform::SessionLogLine("Watch ticket: match " + std::to_string(ticket.match) + " (" + ticket.release + ")");
            Notice(texts::T("launcher.replay.starting", {{"name", PathUtf8(path.filename())}}), NoticeKind::Info);
            return;
        }
        if (!error.empty() || !replay::DecodeFile(bytes.data(), bytes.size(), info, stream, error)
            || !replay::ReadSetup(stream.data(), stream.size(), setup, error))
        {
            Notice(texts::T("launcher.replay.unreadable", {{"error", error}}), NoticeKind::Error);
            return;
        }
        DiscInfo needs{};
        needs.game_id = info.game_id;
        needs.revision = std::uint8_t(info.revision);
        pending_replay_ = PendingReplay{path, needs, {}};
        platform::SessionLogLine("Replay asked: " + PathUtf8(path) + " (" + info.game_id + " revision "
                                 + std::to_string(info.revision) + ", recorded by " + info.build + ")");
        Notice(texts::T("launcher.replay.starting", {{"name", PathUtf8(path.filename())}}), NoticeKind::Info);
    }

    void PollReplay(bool& quit)
    {
        if (!pending_replay_ || ReplayReady() || frontend_launch_) return;
        const auto& wanted = pending_replay_->needs;
        const bool any_revision = pending_replay_->any_revision;
        const auto fits = [&](const DiscInfo& info) {
            return (any_revision ? info.game_id.compare(0, 4, wanted.game_id) == 0
                                 : info.game_id == wanted.game_id && info.revision == wanted.revision)
                && SupportedRelease(info);
        };
        // The chosen image's check first.
        if (disc_check_.Busy() || search_.Busy()) return;
        fs::path image;
        if (check_.info && check_.selection == DiscSelection() && fits(*check_.info) && PinnedMissing().empty())
            image = EffectiveLaunch().disc_path;
        else
        {
            for (const auto& entry : library_)
                if (entry.probe.kind == DiscProbe::Kind::Charged && fits(entry.probe.info))
                {
                    image = entry.path;
                    break;
                }
            if (image.empty())
            {
                if (library_scan_.Busy()) return;
                Notice(texts::T("launcher.replay.no_image", {{"release", RegionRevision(wanted)},
                                                             {"id", wanted.game_id}}),
                       NoticeKind::Error);
                pending_replay_.reset();
                return;
            }
        }
        pending_replay_->image = image;
        if (dirty_) play_prompt_ = true;
        else Start(quit);
    }

    // What keeps the game from starting: a replay only needs the image it found.
    std::string StartBlocker() const { return ReplayReady() ? std::string() : PlayBlocker(); }

    // ------------------------------------------------------------- layout
    void Draw(bool& quit)
    {
        PollKept();
        PollReplay(quit);
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
        if (training_requested_)
        {
            training_requested_ = false;
            Play(quit, true);
        }
        if (competitive_go_)
        {
            competitive_go_ = false;
            StartSet(quit);
        }
        DrawClosePrompt(quit);
        DrawPlayPrompt(quit);
        DrawImportPrompt();
        DrawSignOutPrompt();
        DrawDiscordChoice();
        DrawProfileImport();
        DrawImportedProfiles();
        DrawDisconnectPrompt();
        DrawRegionPrompt();
        DrawChallengeDialog();
        DrawWelcome();
        DrawUpdateDialog();
        DrawUpdateProblem();
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
        // The name in plain text: the logo itself is on the Play and About pages.
        float y = Dp(30);
        DrawTracked(draw, fonts.caption, {pad, y}, Col(color::muted), texts::T("launcher.sidebar.overline"), Dp(2.4f));
        y += FontSize(fonts.caption) + Dp(2);
        DrawLabel(draw, fonts.title, {pad, y}, IM_COL32_WHITE, "RECHARGED");
        y += FontSize(fonts.title) + Dp(4);
        DrawLabel(draw, fonts.caption, {pad, y}, Col(color::dim), "Mario Strikers Charged");
        y += FontSize(fonts.caption) + Dp(28);

        // A group's row opens the last of its pages shown (kNavGroups).
        const int open_group = NavGroupOf(page_);
        if (open_group >= 0) group_page_[open_group] = page_;
        // The pages above the version line: their rows get lower when the
        // window is too low for all of them at the usual height, with room
        // for the open group's panel (each group's pages share its row).
        const float badge = height - Dp(40);
        const float rows = float(PageCount - NavGroupedRows());
        const float row = std::clamp((badge - Dp(8) - y - NavGroupPagesHeight(NavGroupMostPages())) / rows, Dp(34),
                                     Dp(48));
        ImGui::SetCursorPos({Dp(12), y});
        for (int i = 0; i < PageCount; ++i)
        {
            ImGui::SetCursorPosX(Dp(12));
            if (const int group = NavGroupOf(i); group >= 0)
            {
                const NavGroupInfo& info = kNavGroups[group];
                if (i != info.first) continue;
                // Online: a dot for the sign-in (green signed in, amber on its
                // way, red not). Its pages show signed out too: Match rules,
                // Sidekicks and the Miis need no sign-in.
                using State = CompetitiveService::State;
                const auto online = competitive_status_.state;
                const bool signed_in = online == State::SignedIn;
                const ImVec4 dot = signed_in ? color::success
                    : online == State::SigningIn || online == State::Connecting ? color::warning
                    : color::danger;
                const bool is_online = group == NavGroupOf(PageOnlineSettings);
                // Without a usable game image Online is greyed out, its pages folded.
                const std::string blocker = is_online ? OnlineBlocker() : std::string();
                std::vector<NavPage> pages;
                if (blocker.empty())
                    for (int p = i; p < i + info.count; ++p) pages.push_back({PageName(p), page_icons[p], page_ == p});
                ImGui::BeginDisabled(!blocker.empty());
                const int clicked = NavGroup(NavGroupName(group), info.icon, open_group == group, pages, width - Dp(24),
                                             row - Dp(4), is_online && competitive_ ? &dot : nullptr,
                                             is_online && signed_in);
                ImGui::EndDisabled();
                if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", blocker.c_str());
                if (clicked == 0) page_ = group_page_[group];
                if (clicked > 0) page_ = i + clicked - 1;
            }
            else if (NavItem(PageName(i), page_icons[i], page_ == i, width - Dp(24), row - Dp(4))) page_ = i;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + Dp(4));
        }

        // Version at the bottom of the sidebar; test builds show a badge above
        // it that opens their pull request.
        const float nav_end = ImGui::GetCursorPosY();
        if (nav_end < badge - Dp(36) && std::string_view(build::channel) == "test")
        {
            ImGui::SetCursorPos({pad - Dp(2), badge - Dp(36)});
            const std::string number = std::to_string(build::pull_request);
            if (TestBuildBadge(texts::T("launcher.sidebar.test_build", {{"number", number}})))
                SDL_OpenURL(BuildNotesUrl(CurrentBuild()).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", texts::T("launcher.sidebar.test_build_about", {{"number", number}}).c_str());
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
        // An input still waiting for its key (a hotkey's) would take the next
        // key pressed on another page, a text box's included.
        if (page_ != PageControls) input_capture_.Cancel();
        if (page_ != PageRules) preset_delete_ = -1;
        // Bluetooth inquiries can disturb other Bluetooth devices: search only
        // while the Controls page is open (never in development captures).
        bluetooth_search_.SetActive(page_ == PageControls && config_ok_ && draft_.bluetooth_search && !capture_);
        // Which Wii Remotes answer: for the Players card, and for the Play
        // page when a player uses a remote.
        if (page_ == PageControls || (page_ == PagePlay && RemotePlayer())) PollWiimotes();
        // The Play page has the bottom bar only for unsaved changes or a problem.
        const bool bar_shown = page_ != PagePlay || dirty_ || !config_ok_ || notice_kind_ == NoticeKind::Warning
            || notice_kind_ == NoticeKind::Error;
        const float bar = bar_shown ? Dp(74) : 0.0f;
        const ImVec2 content{size.x, size.y - bar};
        // An Online page without a usable game image (it went away): the Game page.
        if (OnlinePage(page_) && !OnlineBlocker().empty()) page_ = PageGame;
        ImGui::SetCursorPos(origin);
        if (page_ == PagePlay) DrawPlayPage(origin, content, quit);
        else
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(36, 30));
            ImGui::BeginChild("##page", content, ImGuiChildFlags_AlwaysUseWindowPadding);
            // The Mii editor opened or closed: the page from its top.
            if (page_scroll_top_)
            {
                ImGui::SetScrollY(0.0f);
                page_scroll_top_ = false;
            }
            const float available = ImGui::GetContentRegionAvail().x;
            const float column = std::min(available, Dp(900));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - column) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            ImGui::BeginChild("##column", {column, 0}, ImGuiChildFlags_AutoResizeY);
            ImGui::BeginDisabled(!config_ok_ && page_ != PageAbout);
            if (mii_editor_.open && mii_editor_.page == page_) MiiEditorPage();
            else if (preset_edit_ && page_ == PageRules) PresetEditorPage();
            else switch (page_)
            {
            case PageGraphics: GraphicsPage(); break;
            case PageGame: GamePage(); break;
            case PageSidekicks: SidekicksPage(); break;
            case PageRules: MatchRulesPage(); break;
            case PageOnlineSettings: OnlineSettingsPage(); break;
            case PageControls: ControlsPage(); break;
            case PageExtras: ExtrasPage(); break;
            case PageReplays: ReplaysPage(); break;
            case PageAdvanced: AdvancedPage(); break;
            default: AboutPage(); break;
            }
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            // Development captures: RECHARGED_CAPTURE_SCROLL=bottom shows the
            // page's end, a number scrolls that far down (in window units; the
            // mouse wheel does nothing without a real window).
            if (const char* scroll = capture_ ? std::getenv("RECHARGED_CAPTURE_SCROLL") : nullptr; scroll && *scroll)
                ImGui::SetScrollY(std::string_view(scroll) == "bottom"
                                      ? ImGui::GetScrollMaxY()
                                      : std::min(Dp(std::strtof(scroll, nullptr)), ImGui::GetScrollMaxY()));
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        if (bar_shown) DrawBottomBar({origin.x, origin.y + content.y}, {size.x, bar}, quit);
    }

    void DrawPlayPage(ImVec2 origin, ImVec2 size, bool& quit)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const float pad = Dp(44);
        // The tagline under the logo, the game in white, the rest softer; on
        // two lines where one does not fit (broken at the first space after
        // the game).
        const std::string game = "Mario Strikers Charged";
        const std::string tagline = texts::T("launcher.play.tagline", {{"game", game}});
        const std::size_t game_at = tagline.find(game);
        const bool named = game_at != std::string::npos;
        const std::string before = named ? tagline.substr(0, game_at) : tagline;
        const std::string rest = named ? tagline.substr(game_at + game.size()) : std::string();
        const float before_width = TextSize(fonts.heading, before.c_str()).x;
        const float game_width = named ? TextSize(fonts.heading, game.c_str()).x : 0.0f;
        const std::size_t break_at = rest.find(' ');
        const float line_width = before_width + game_width + TextSize(fonts.heading, rest.c_str()).x;
        const float two_width = std::max(
            before_width + game_width + TextSize(fonts.heading, rest.substr(0, break_at).c_str()).x,
            break_at == std::string::npos ? 0.0f : TextSize(fonts.heading, rest.c_str() + break_at + 1).x);

        // From the page's corners: on the top line the players online and the
        // update controls; in the bottom right corner the Play button with the
        // status at its left, both ending on the bottom line; at the bottom
        // left the title: the tagline on that line when it fits beside the
        // status, otherwise just above that row, the logo above the tagline,
        // as large as the room allows under the server's message, which runs
        // across the page under the top line.
        const float margin = Dp(42), button = Dp(62), button_width = Dp(230);
        const float title_gap = Dp(10), row_gap = Dp(28), message_gap = Dp(22);
        const float smallest = Dp(201) / logo_aspect_, largest = Dp(385) / logo_aspect_;
        const float left = origin.x + pad, right = origin.x + size.x - pad;
        const float bottom = origin.y + size.y - margin;
        const float button_x = right - button_width, button_y = bottom - button;
        const float status_right = button_x - Dp(28);
        const std::string blocker = PlayBlocker();
        const ImVec2 status = DrawPlayStatus(blocker, status_right, bottom, left, false);
        const bool message = ServerMessageShown();
        const float top = origin.y + Dp(84); // under the top line (which ends at 56), with air
        const float above = message ? message_height_ + message_gap : Dp(4);
        // Room beside the status on the bottom line, for the tagline (and the
        // logo where the status reaches up beside it).
        const float beside = (status.x < status_right ? status.x : button_x) - Dp(32) - left;
        const auto logo_for = [&](float logo_bottom) {
            return std::clamp(logo_bottom - top - above, smallest, largest);
        };
        bool low = false, one_line = line_width <= size.x * 0.75f - pad;
        for (const bool one : {true, false})
        {
            const float width = one ? line_width : two_width;
            if (width > beside) continue;
            const float subtitle = one ? FontSize(fonts.heading) : FontSize(fonts.heading) * 2 + Dp(2);
            const float logo_bottom = bottom - subtitle - title_gap;
            if (status.y < logo_bottom && logo_for(logo_bottom) * logo_aspect_ > beside + Dp(8)
                && smallest * logo_aspect_ > beside + Dp(8))
                continue;
            low = true;
            one_line = one;
            break;
        }
        const float subtitle = one_line ? FontSize(fonts.heading) : FontSize(fonts.heading) * 2 + Dp(2);
        const float title_bottom = low ? bottom : std::min(button_y, status.y) - row_gap;
        const float logo_bottom = title_bottom - subtitle - title_gap;
        float logo = logo_for(logo_bottom);
        // On the bottom line a logo reaching down beside the status keeps left of it.
        if (low && status.y < logo_bottom) logo = std::max(smallest, std::min(logo, (beside + Dp(8)) / logo_aspect_));
        float y = logo_bottom - logo;
        // The artwork covers the top of the page and fades out at about two
        // thirds of it (the logo and the Play row stand on the page below).
        const float art = std::floor(size.y * 0.7f);
        hero_ = {origin.x, origin.y, size.x, art};
        hero_visible_ = true;
        const ImVec2 art_max{origin.x + size.x, origin.y + art};
        // Shades of the page colour over the artwork, eased in eight bands so that no
        // edge shows: the logo's side dark, the lower part dissolving into the page
        // towards the bottom, a little shade under the top line.
        const auto fade = [&](ImVec2 min, ImVec2 max, float from, float to, bool across) {
            constexpr int bands = 8;
            const auto ease = [](float t) { return t * t * (3.0f - 2.0f * t); };
            for (int i = 0; i < bands; ++i)
            {
                const float t0 = float(i) / bands, t1 = float(i + 1) / bands;
                const ImU32 a = Col(color::window, from + (to - from) * ease(t0));
                const ImU32 b = Col(color::window, from + (to - from) * ease(t1));
                if (across)
                    draw->AddRectFilledMultiColor({min.x + (max.x - min.x) * t0, min.y},
                                                  {min.x + (max.x - min.x) * t1, max.y}, a, b, b, a);
                else
                    draw->AddRectFilledMultiColor({min.x, min.y + (max.y - min.y) * t0},
                                                  {max.x, min.y + (max.y - min.y) * t1}, a, a, b, b);
            }
        };
        const float dark = origin.x + size.x * 0.18f;
        draw->AddRectFilled(origin, {dark, art_max.y}, Col(color::window, 0.94f));
        fade({dark, origin.y}, {origin.x + size.x * 0.66f, art_max.y}, 0.94f, 0.0f, true);
        fade({origin.x, origin.y + art * 0.46f}, {art_max.x, art_max.y - Dp(8)}, 0.0f, 1.0f, false);
        draw->AddRectFilled({origin.x, art_max.y - Dp(8)}, art_max, Col(color::window));
        fade(origin, {art_max.x, origin.y + Dp(90)}, 0.35f, 0.0f, false);

        // A soft glow from the logo's lit ball, so that the logo sits in the
        // scene, brighter while the sparks crackle.
        const ImVec2 ball{origin.x + pad - Dp(8) + logo * logo_aspect_ * 0.852f, y + logo * 0.232f};
        const float glow = 0.006f * (1 + Sizzle());
        for (int i = 20; i >= 1; --i)
            draw->AddCircleFilled(ball, logo * 0.055f * float(i), Col(color::logo, glow), 48);
        DrawLogo(draw, {origin.x + pad - Dp(8), y}, logo);
        y += logo + title_gap;
        const ImU32 soft = Col(color::text, 0.86f);
        const std::size_t space = one_line ? std::string::npos : break_at;
        if (!before.empty()) DrawLabelShadowed(draw, fonts.heading, {left, y}, soft, before.c_str(), 1.2f);
        if (named)
            DrawLabelShadowed(draw, fonts.heading, {left + before_width, y}, IM_COL32_WHITE, game.c_str(), 1.2f);
        DrawLabelShadowed(draw, fonts.heading, {left + before_width + game_width, y}, soft,
                          rest.substr(0, space).c_str(), 1.2f);
        if (space != std::string::npos)
            DrawLabelShadowed(draw, fonts.heading, {left, y + FontSize(fonts.heading) + Dp(2)}, soft,
                              rest.c_str() + space + 1, 1.2f);
        DrawUpdateBar(origin, size.x, pad);
        // How many play online, opposite the update controls (as tall, on their line).
        DrawPlayersOnline(left, origin.y + Dp(18), Dp(38));

        // The Play button and, at its left, the status.
        ImGui::SetCursorScreenPos({button_x, button_y});
        if (PrimaryButton("play", texts::T("launcher.play.button"), {button_width, button}, blocker.empty(),
                          Icon::Play, true))
            Play(quit);
        if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", blocker.c_str());
        DrawPlayStatus(blocker, status_right, bottom, left);

        // The server's message under the top line, as wide as the page (in a
        // wide window no wider than a line reads well).
        if (message) ServerMessageBanner({left, top}, std::min(right - left, Dp(900)));
        else
            message_height_ = 0.0f;
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
        int page; // the page it opens, -1 for none
    };

    // A note's pill at most `max_width` wide: its text (elided when needed),
    // whether the secondary part fits too, and its width.
    struct PillLayout
    {
        std::string text;
        std::string secondary; // empty when left out
        float width = 0.0f;
    };
    PillLayout MeasurePill(const StatusNote& note, float max_width) const
    {
        const auto& fonts = CurrentFonts();
        const float left = Dp(30), right = Dp(26);
        const float room = std::max(Dp(40), max_width - left - right);
        const std::string secondary = note.secondary.empty() ? std::string() : "  \xC2\xB7  " + note.secondary;
        const bool both = !secondary.empty() && TextSize(fonts.caption, (note.text + secondary).c_str()).x <= room;
        PillLayout layout{Elide(fonts.caption, note.text, room), both ? secondary : std::string(), 0.0f};
        layout.width = left + TextSize(fonts.caption, layout.text.c_str()).x
            + (both ? TextSize(fonts.caption, secondary.c_str()).x : 0.0f) + right;
        return layout;
    }

    // At most `max_width` wide; true when clicked.
    bool StatusPill(const char* id, const StatusNote& note, float max_width)
    {
        const auto& fonts = CurrentFonts();
        const float height = Dp(28), left = Dp(30);
        const PillLayout layout = MeasurePill(note, max_width);
        const std::string& text = layout.text;
        const std::string& secondary = layout.secondary;
        const bool both = !secondary.empty();
        const ImVec2 label = TextSize(fonts.caption, text.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size{layout.width, height};
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
        if (note.page >= 0)
            DrawChevron(draw, {max.x - Dp(14) + Dp(2) * hover, middle}, Dp(10), Col(Mix(color::dim, color::text, hover)));
        if (hovered) ImGui::SetTooltip("%s", note.tooltip.c_str());
        return pressed;
    }

    // Left of the Play button, ending on its bottom line (its right edge at
    // `right`, its bottom at `bottom`, never left of `leftmost`): whether the
    // game can start and, under that, what to know first, each opening its
    // page: the image it starts, a controller that is not connected and who
    // plays instead, the gameplay tweaks that are on. Returns its top left
    // corner; `paint` false only measures.
    ImVec2 DrawPlayStatus(const std::string& blocker, float right, float bottom, float leftmost, bool paint = true)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        ImVec4 tint = color::success;
        Icon icon = Icon::Check;
        bool spinner = false;
        std::string headline; // none when ready: the green button says it
        std::string detail;
        const bool missing = !PinnedMissing().empty() && !disc_check_.Busy() && !search_.Busy();
        if (disc_check_.Busy() || search_.Busy())
        {
            tint = color::info;
            icon = Icon::None;
            spinner = true;
            headline = search_.Busy() ? texts::T("launcher.play.status.searching")
                                  : texts::T("launcher.play.status.checking");
            detail = search_.Busy() ? texts::T("launcher.play.status.searching_where")
                                : texts::T("launcher.play.status.checking_what");
        }
        else if (missing)
        {
            // Never another image instead: the player decides.
            tint = color::warning;
            icon = Icon::Warning;
            const auto info = PinnedInfo();
            headline = texts::T("launcher.play.status.missing");
            const std::string name = PathUtf8(PathFromUtf8(DiscSelection()).filename());
            detail = (info ? RegionRevision(*info) + "  \xC2\xB7  " : std::string())
                + (check_.folder_missing ? texts::T("launcher.play.status.drive_missing", {{"name", name}})
                                         : texts::T("launcher.play.status.file_missing", {{"name", name}}));
        }
        else if (!blocker.empty())
        {
            tint = DiscSelection().empty() ? color::info : color::warning;
            icon = DiscSelection().empty() ? Icon::Disc : Icon::Warning;
            headline = DiscSelection().empty() ? texts::T("launcher.play.status.choose_image")
                                               : texts::T("launcher.play.status.not_ready");
            detail = blocker;
        }
        const bool ready = !disc_check_.Busy() && !search_.Busy() && !missing && blocker.empty() && check_.info;
        std::vector<StatusNote> notes;
        if (blocker.empty())
        {
            // The image it starts, which the Game page changes.
            if (ready)
                notes.push_back({Icon::Disc, color::success, false, RegionRevision(*check_.info),
                                 check_.info->game_id,
                                 ReleaseTitle(*check_.info) + "\n" + PathUtf8(PathFromUtf8(DiscSelection()).filename()),
                                 PageGame});
            // A player's controller that is not connected, and who plays instead.
            const auto lineup = StartLineup();
            const auto brief = MissingControllersBrief(draft_.players, lineup);
            if (!brief.missing.empty())
            {
                const auto missing_count = std::count(lineup.missing.begin(), lineup.missing.end(), true);
                notes.push_back({Icon::Gamepad, color::warning, true, brief.missing, brief.instead,
                                 MissingControllersNote(draft_.players, lineup) + "\n"
                                     + texts::Count("launcher.play.notes.connect_controllers", missing_count),
                                 PageControls});
            }
            // Gameplay tweaks change the original game: never silently (the
            // development switch for the menus changes no gameplay).
            std::string names;
            int count = 0;
            for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
                if (draft_.tweaks[n] && kGameTweaks[n].tweak != GameTweak::OverrideRetailMenus)
                {
                    names += std::string("\n\xE2\x80\xA2 ") + texts::TweakName(kGameTweaks[n]);
                    ++count;
                }
            if (count)
                notes.push_back({Icon::Sliders, color::accent, false,
                                 texts::Count("launcher.play.notes.tweaks_on", count), "",
                                 texts::T("launcher.play.notes.tweaks_on_list") + names, -1});
        }
        // As wide as its widest line, at most `most`; its lines start at its
        // left edge (the headline after its icon), the block ends at `right`.
        const float most = std::max(Dp(160), std::min(right - leftmost, Dp(460)));
        const float inset = Dp(30);
        const float wrap = most - inset;
        const float caption = FontSize(fonts.caption);
        const ImVec2 detail_size =
            detail.empty() ? ImVec2{} : fonts.caption->CalcTextSizeA(caption, FLT_MAX, wrap, detail.c_str());
        // The notes side by side under the headline, in as many rows as the
        // width needs: with one row the status is as tall as the button, the
        // row ending with it.
        const float pill = Dp(28), pill_gap = Dp(6), pill_top = Dp(8), pill_space = Dp(8);
        std::vector<ImVec2> places; // x from the status's left edge, row
        int rows = 0;
        float x = 0.0f, widest = 0.0f;
        for (const auto& note : notes)
        {
            const float width = MeasurePill(note, most).width;
            if (rows == 0 || (x > 0.0f && x + width > most))
            {
                x = 0.0f;
                ++rows;
            }
            places.push_back({x, float(rows - 1)});
            widest = std::max(widest, x + width);
            x += width + pill_space;
        }
        // Ready to play, the green button says it: only the notes then.
        const bool quiet = blocker.empty() && !disc_check_.Busy() && !search_.Busy() && !missing;
        float width = std::max({quiet ? 0.0f : inset + TextSize(fonts.label, headline.c_str()).x,
                                inset + detail_size.x, widest});
        if (missing) width = most;
        width = std::min(width, most);
        float height = quiet ? 0.0f : Dp(26) + detail_size.y;
        if (rows) height += (quiet ? 0.0f : pill_top) + float(rows) * (pill + pill_gap) - pill_gap;
        if (missing) height += Dp(4) + Dp(32);
        const ImVec2 pos{std::floor(right - width), std::floor(bottom - height)};
        if (!paint) return pos;
        const float text_x = pos.x + inset;

        if (!quiet)
        {
            if (spinner) Spinner({pos.x + Dp(11), pos.y + Dp(12)}, Dp(8), Col(tint));
            else if (icon != Icon::None) DrawIcon(draw, icon, {pos.x + Dp(11), pos.y + Dp(12)}, Dp(20), Col(tint));
            DrawLabel(draw, fonts.label, {text_x, pos.y + Dp(1)}, Col(color::text), headline.c_str());
        }
        // Where the detail goes, the pills a little lower (quiet: at the top).
        float y = quiet ? pos.y - pill_top : pos.y + Dp(26);
        if (!detail.empty())
        {
            draw->AddText(fonts.caption, caption, {text_x, y}, Col(color::muted), detail.c_str(), nullptr, wrap);
            y += detail_size.y;
        }
        // The pills (and the missing image's buttons) start at the status's left
        // edge, under the icon, so that a pill's text lines up with the headline.
        ImGui::PushID("play_status");
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            const float left = pos.x + places[i].x;
            ImGui::SetCursorScreenPos({left, y + pill_top + places[i].y * (pill + pill_gap)});
            ImGui::PushID(int(i));
            if (StatusPill("##note", notes[i], pos.x + most - left) && notes[i].page >= 0) page_ = notes[i].page;
            ImGui::PopID();
        }
        ImGui::PopID();
        if (missing)
        {
            ImGui::SetCursorScreenPos({pos.x, y + Dp(4)});
            MissingImageActions(right);
        }
        return pos;
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
        const char* locate = texts::T("launcher.play.missing.locate");
        if (GhostButton("locate", locate, place(locate, Icon::Folder), !DialogActive(), Icon::Folder)) Locate();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("launcher.play.missing.locate_about"));
        const auto info = PinnedInfo();
        const auto copies = info ? SameRelease(library_, info->game_id, info->revision, PathFromUtf8(DiscSelection()))
                                 : std::vector<const LibraryImage*>{};
        if (!copies.empty())
        {
            const auto* copy = copies.front();
            const char* use = texts::T("launcher.play.missing.use_copy");
            if (GhostButton("copy", use, place(use, Icon::Check), true, Icon::Check))
                UseImage(copy->path, copy->probe.info, copy->size);
            if (ImGui::IsItemHovered())
            {
                const std::string tip = texts::T("launcher.play.missing.use_copy_about",
                                                 {{"release", RegionRevision(copy->probe.info)},
                                                  {"path", PathUtf8(copy->path)}});
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        const char* another = texts::T("launcher.play.missing.choose_another");
        if (GhostButton("another", another, place(another, Icon::Disc), true, Icon::Disc)) page_ = PageGame;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("launcher.play.missing.choose_another_about"));
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
            if (PrimaryButton("bar_play", texts::T("launcher.bottom_bar.play"), {Dp(140), button_h}, blocker.empty(),
                              Icon::Play))
                Play(quit);
            if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", blocker.c_str());
            x -= Dp(12);
        }
        x -= Dp(118);
        ImGui::SetCursorScreenPos({x, button_y});
        const char* save = dirty_ ? texts::T("launcher.bottom_bar.save") : texts::T("launcher.bottom_bar.saved");
        if (SecondaryButton("save", save, {Dp(118), button_h}, config_ok_ && (dirty_ || !file_.exists),
                            dirty_ ? Icon::Check : Icon::None))
            Save();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s",
                              texts::T("launcher.bottom_bar.save_about", {{"path", PathUtf8(file_.path)}}).c_str());
        x -= Dp(10) + Dp(104);
        ImGui::SetCursorScreenPos({x, button_y});
        if (SecondaryButton("revert", texts::T("launcher.bottom_bar.revert"), {Dp(104), button_h},
                            dirty_ || !config_ok_, Icon::Refresh))
            Reload();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", texts::T("launcher.bottom_bar.revert_about"));
        // The welcome guide again, as an icon beside the settings' actions.
        if (!welcome_.empty())
        {
            x -= Dp(10) + button_h;
            ImGui::SetCursorScreenPos({x, button_y});
            if (GhostButton("welcome", "", {button_h, button_h}, true, Icon::Bulb)) welcome_pending_ = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("launcher.bottom_bar.welcome_guide"));
        }
        float right = x - Dp(16);
        if (dirty_)
        {
            const char* unsaved = texts::T("launcher.bottom_bar.unsaved");
            const float chip = TextSize(fonts.caption, unsaved).x + Dp(20);
            right -= chip;
            ImGui::SetCursorScreenPos({right, origin.y + (size.y - Dp(26)) * 0.5f});
            Chip(unsaved, color::warning);
            right -= Dp(14);
        }

        // Status message on the left.
        ImVec4 tint = color::muted;
        Icon icon = Icon::Info;
        switch (notice_kind_)
        {
        case NoticeKind::Success: tint = color::success; icon = Icon::Check; break;
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
            TextColored(fonts.heading, color::text, texts::T("launcher.region_prompt.title"));
            ImGui::Dummy({0, Dp(4)});
            TextWrappedColored(fonts.body, color::muted, UnplayableReason(region_prompt_info_).c_str());
            ImGui::Dummy({0, Dp(4)});
            const std::string runs =
                texts::T("launcher.region_prompt.releases", {{"releases", PlayableReleasesText()}});
            TextWrappedColored(fonts.body, color::text, runs.c_str());
            ImGui::Dummy({0, Dp(10)});
            if (PrimaryButton("region_ok", texts::T("launcher.region_prompt.ok"), Dp(120, 42), true) || CancelPressed()
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
            TextColored(fonts.heading, color::text, texts::T("launcher.play_prompt.title"));
            TextColored(fonts.body, color::muted, texts::T("launcher.play_prompt.unsaved"));
            ImGui::Dummy({0, Dp(8)});
            if (PrimaryButton("save_play", texts::T("launcher.play_prompt.save_play"), Dp(170, 42), true, Icon::Play))
            {
                ImGui::CloseCurrentPopup();
                if (Save() && StartBlocker().empty()) Start(quit);
                pending_replay_.reset(); // a replay that did not start is not kept for the next Play
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("discard_play", texts::T("launcher.play_prompt.discard_play"), Dp(170, 42)))
            {
                ImGui::CloseCurrentPopup();
                // Back to the saved settings; start only if they can still play.
                Reload();
                if (config_ok_ && StartBlocker().empty()) Start(quit);
                else
                    Notice(texts::T("launcher.play_prompt.not_started",
                                    {{"reason", config_ok_ ? StartBlocker() : std::string(texts::T("launcher.play_prompt.settings_problem"))}}),
                           NoticeKind::Warning);
                pending_replay_.reset();
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("cancel_play", texts::T("launcher.play_prompt.cancel"), Dp(100, 42)) || CancelPressed())
            {
                ImGui::CloseCurrentPopup();
                pending_replay_.reset();
            }
            ImGui::EndPopup();
        }
        // Another dialog closed the question unanswered (ImGui keeps one modal
        // at a time): its replay doesn't wait for the next Play.
        else if (ReplayReady() && !play_prompt_)
            pending_replay_.reset();
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
            TextColored(fonts.heading, color::text, texts::T("launcher.close_prompt.title"));
            TextColored(fonts.body, color::muted, texts::T("launcher.close_prompt.unsaved"));
            ImGui::Dummy({0, Dp(8)});
            if (PrimaryButton("save_close", texts::T("launcher.close_prompt.save_close"), Dp(170, 42), true))
            { if (Save()) quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("discard", texts::T("launcher.close_prompt.discard"), Dp(110, 42)))
            { quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("cancel", texts::T("launcher.close_prompt.cancel"), Dp(100, 42)) || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // -------------------------------------------------------------- pages
    // The game itself: its images, its language and its saves.
    void GamePage()
    {
        PageHeader(texts::T("launcher.game.title"), texts::T("launcher.game.about"));
        if (options_.launch.disc)
        {
            Chip(texts::T("launcher.game.command_line"), color::info, Icon::Info);
            ImGui::Dummy({0, Dp(2)});
        }

        GameImagesCard();

        BeginCard("##language", texts::T("launcher.game.language.title"), texts::T("launcher.game.language.about"),
                  Icon::Info);
        if (BeginRows("##language_rows"))
        {
            Row(texts::T("launcher.game.language.text_language"),
                texts::T("launcher.game.language.text_language_about"));
            LanguageControl();
            EndRows();
        }
        EndCard();

        SaveDataCard();
    }

    // How the game opens and what draws it. The picture's own settings (VSync,
    // frame rate, picture style...) change while the game runs, in its
    // OPTIONS > SETTINGS > DISPLAY.
    void GraphicsPage()
    {
        PageHeader(texts::T("launcher.graphics.title"), texts::T("launcher.graphics.about"));
        if (options_.launch.size || options_.launch.fullscreen || options_.launch.aspect || options_.launch.frame_rate)
        {
            Chip(texts::T("launcher.game.command_line"), color::info, Icon::Info);
            ImGui::Dummy({0, Dp(2)});
        }
        ScreenCard();
        RendererCard();
    }

    // What the game opens with: the window's size, the fullscreen resolution,
    // the display and the antialiasing.
    void ScreenCard()
    {
        BeginCard("##screen", texts::T("launcher.graphics.screen.title"), texts::T("launcher.graphics.screen.about"),
                  Icon::Display);
        if (BeginRows("##screen_rows"))
        {
            Row(texts::T("launcher.graphics.screen.window_size.name"),
                draft_.remember_window ? texts::T("launcher.graphics.screen.window_size.about_remembered")
                                       : texts::T("launcher.graphics.screen.window_size.about"));
            WindowSizeControl();
            Row(texts::T("launcher.graphics.screen.fullscreen_resolution.name"),
                texts::T("launcher.graphics.screen.fullscreen_resolution.about"));
            FullscreenResolutionControl();
            if (displays_.size() > 1 || draft_.monitor > 0)
            {
                Row(texts::T("launcher.graphics.screen.display.name"),
                    texts::T("launcher.graphics.screen.display.about"));
                DisplayChoice();
            }
            Row(texts::T("launcher.graphics.screen.antialiasing.name"),
                texts::T("launcher.graphics.screen.antialiasing.about"));
            AntialiasingKindChoice();
            EndRows();
        }
        EndCard();
    }

    // What draws the game: the graphics API (a choice on Windows only), the
    // graphics card (macOS has one) and the shaders compiled for them. What
    // helps when the game does not start or shows glitches.
    void RendererCard()
    {
        BeginCard("##renderer", texts::T("launcher.graphics.renderer.title"),
                  texts::T("launcher.graphics.renderer.about"), Icon::Layers);
        if (BeginRows("##renderer_rows"))
        {
#if defined(_WIN32)
            // Automatic is its own choice: only it may switch to DirectX 12.
            Row(texts::T("launcher.graphics.api.name"), texts::T("launcher.graphics.api.about"));
            const char* automatic = texts::T("launcher.graphics.api.automatic");
            const char* preview = draft_.backend == "auto" ? automatic : texts::T("launcher.graphics.api.choose");
            for (const auto& choice : kGraphicsBackends)
                if (draft_.backend == choice.value) preview = choice.label;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##graphics_api", preview))
            {
                if (ImGui::Selectable(StableLabel(automatic, "auto").c_str(), draft_.backend == "auto"))
                { draft_.backend = "auto"; dirty_ = true; }
                for (const auto& choice : kGraphicsBackends)
                    if (ImGui::Selectable(choice.label, draft_.backend == choice.value))
                    { draft_.backend = choice.value; dirty_ = true; }
                ImGui::EndCombo();
            }
#endif
#if !defined(__APPLE__)
            GraphicsCardRow();
#endif
            Row(texts::T("launcher.graphics.shader_cache.name"), texts::T("launcher.graphics.shader_cache.about"));
            if (SecondaryButton("clear_shader_cache", texts::T("launcher.graphics.shader_cache.clear"),
                                {ImGui::GetContentRegionAvail().x, Dp(40)}, true, Icon::Refresh))
            {
                try
                {
                    const auto cleared = platform::ClearShaderCache(platform::ShaderCacheDirectory(base_));
                    Notice(cleared.removed_files ? texts::T("launcher.graphics.shader_cache.cleared")
                                                 : texts::T("launcher.graphics.shader_cache.empty"),
                           NoticeKind::Success);
                }
                catch (const std::exception& error) { Notice(error.what(), NoticeKind::Error); }
            }
            EndRows();
        }
        EndCard();
    }

    // Advanced -> Menu language ([interface] language): Automatic or one of
    // the languages of assets/lang, each by its own name. The launcher changes
    // to it at once.
    void MenuLanguageControl()
    {
        const char* automatic = texts::T("settings.advanced.interface.menu_language.automatic");
        const auto named = [this](const std::string& code) -> const texts::LanguageChoice* {
            for (const auto& choice : menu_languages_)
                if (choice.code == code) return &choice;
            return nullptr;
        };
        const auto* chosen = named(draft_.interface_language);
        const std::string preview = draft_.interface_language == "auto" ? std::string(automatic)
            : chosen                                                     ? chosen->name
                                                                         : draft_.interface_language;
        std::string picked;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##menu_language", preview.c_str()))
        {
            if (ImGui::Selectable((std::string(automatic) + "###auto").c_str(), draft_.interface_language == "auto"))
                picked = "auto";
            for (const auto& choice : menu_languages_)
                if (ImGui::Selectable((choice.name + "###" + choice.code).c_str(),
                                      choice.code == draft_.interface_language))
                    picked = choice.code;
            ImGui::EndCombo();
        }
        if (!picked.empty() && picked != draft_.interface_language)
        {
            draft_.interface_language = picked;
            dirty_ = true;
            LoadTexts();
        }
        const auto& fonts = CurrentFonts();
        if (draft_.interface_language == "auto")
        {
            const auto* found = named(texts::Language());
            const std::string text = texts::T("settings.advanced.interface.menu_language.automatic_is",
                                              {{"language", found ? found->name : texts::Language()}});
            TextWrappedColored(fonts.caption, color::muted, text.c_str());
        }
        else if (!named(draft_.interface_language))
        {
            const std::string text =
                texts::T("settings.advanced.interface.menu_language.missing", {{"language", draft_.interface_language}});
            TextWrappedColored(fonts.caption, color::warning, text.c_str());
        }
    }

    // Where the game keeps its Wii storage, and importing a save from Dolphin.
    void SaveDataCard()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##data", texts::T("launcher.game.saves.title"), texts::T("launcher.game.saves.about"), Icon::Folder);
        bool browsing;
        { std::lock_guard<std::mutex> lock(folder_dialog_->mutex); browsing = folder_dialog_->active; }
        const float buttons = Dp(118) + Dp(10) + Dp(118);
        const std::string default_folder = PathUtf8(DefaultSaveFolder(base_));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - Dp(10));
        const std::string hint = texts::T("launcher.game.saves.default_hint", {{"folder", default_folder}});
        if (ImGui::InputTextWithHint("##save_folder", hint.c_str(), &draft_.save_folder)) dirty_ = true;
        ImGui::SameLine(0, Dp(10));
        if (SecondaryButton("browse_saves", texts::T("launcher.game.saves.browse"), {Dp(118), ImGui::GetFrameHeight()},
                            !browsing && config_ok_, Icon::Folder))
            BrowseFolder(FolderPurpose::SaveFolder);
        ImGui::SameLine(0, Dp(10));
        if (SecondaryButton("default_saves", texts::T("launcher.game.saves.default"),
                            {Dp(118), ImGui::GetFrameHeight()}, config_ok_ && !draft_.save_folder.empty(),
                            Icon::Refresh))
        {
            draft_.save_folder.clear();
            dirty_ = true;
        }
        if (draft_.save_folder != file_.settings.save_folder)
        {
            ImGui::Dummy({0, Dp(2)});
            TextWrappedColored(fonts.caption, color::warning, texts::T("launcher.game.saves.new_folder"));
        }

        const fs::path folder = SaveFolder();
        if (BeginRows("##data_rows"))
        {
            const std::string path = PathUtf8(folder);
            Row(texts::T("launcher.game.saves.folder_in_use"), path.c_str());
            if (SecondaryButton("open_data", texts::T("launcher.game.saves.open_folder"),
                                {ImGui::GetContentRegionAvail().x, Dp(40)}, fs::exists(folder), Icon::Folder))
                OpenPath(folder);
            if (!fs::exists(folder) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", texts::T("launcher.game.saves.created_later"));

            const auto found = mscharged::platform::FindDolphinSaves(Release().title_id);
            const std::string description = found.empty()
                ? std::string(texts::T("launcher.game.saves.import_about"))
                : texts::T("launcher.game.saves.import_found", {{"path", PathUtf8(found.front())}});
            Row(texts::T("launcher.game.saves.import"), description.c_str());
            const float width = ImGui::GetContentRegionAvail().x;
            if (!found.empty())
            {
                if (SecondaryButton("import_dolphin", texts::T("launcher.game.saves.import_dolphin"), {width, Dp(40)},
                                    config_ok_, Icon::Copy))
                    AskImport(found.front(), ImportKind::Save);
                ImGui::Dummy({0, Dp(2)});
            }
            if (SecondaryButton("import_choose",
                                found.empty() ? texts::T("launcher.game.saves.choose_folder")
                                              : texts::T("launcher.game.saves.choose_another_folder"),
                                {width, Dp(40)}, !browsing && config_ok_, Icon::Folder))
                BrowseFolder(FolderPurpose::DolphinSave);
            EndRows();
        }
        EndCard();
    }

    // ------------------------------------------------------------- Mii heads
    // The Miis' heads: the game image's Mii parts read and heads drawn on a
    // worker (MiiFaces), shown as SDL textures.
    void StartMiiFaces()
    {
        MiiFaces::Hooks hooks;
        hooks.make = [renderer = renderer_](const platform::mii::RgbaImage& image) -> MiiFaces::Texture {
            SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                                     image.width, image.height);
            if (!texture) return nullptr;
            if (!SDL_UpdateTexture(texture, nullptr, image.pixels.data(), image.width * 4))
            {
                SDL_DestroyTexture(texture);
                return nullptr;
            }
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            return texture;
        };
        hooks.free = [](MiiFaces::Texture texture) { SDL_DestroyTexture(static_cast<SDL_Texture*>(texture)); };
        mii_faces_ = std::make_unique<MiiFaces>(std::move(hooks));
    }

    // Every frame, before drawing: the game image in use (its parts draw the
    // heads, read when the first head is asked for) and the heads drawn since.
    // An image without parts is logged once; its Miis keep their figures.
    void UpdateMiiFaces()
    {
        if (!mii_faces_) return;
        // Miis show on the Online pages only.
        if (OnlinePage(page_)) mii_faces_->SetImage(config_ok_ ? EffectiveLaunch().disc_path : fs::path());
        mii_faces_->Update();
        const bool failed = mii_faces_->PartsState() == MiiFaces::Parts::Failed;
        if (failed && !mii_faces_failed_)
            platform::SessionLogLine("Mii faces: the game image's Mii parts can't be read: " + mii_faces_->Error());
        mii_faces_failed_ = failed;
    }

    // The pixel size a head is drawn at for `size` window units: 64, 128 or
    // 256, whichever covers it on this display.
    static int HeadPixels(float size)
    {
        const float pixels = size * std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
        return pixels <= 64.5f ? 64 : pixels <= 128.5f ? 128 : 256;
    }

    static ImTextureID TextureId(MiiFaces::Texture texture)
    {
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
    }

    // A Mii in a disc `size` wide: its head on its favourite colour, drawn
    // from its 74-byte record with the game image's Mii parts as the Wii draws
    // its Mii icons; until the head is drawn (or without the parts, or a
    // record) its figure on that colour.
    void DrawMii(ImDrawList* draw, ImVec2 center, float size, int favorite_color, const std::string& record,
                 int priority = 1)
    {
        const MiiFaces::Texture head = mii_faces_ && record.size() == platform::mii::kMiiRecordBytes
            ? mii_faces_->Head(record, {HeadPixels(size)}, priority)
            : nullptr;
        if (!head)
        {
            DrawMiiFigure(draw, center, size, favorite_color);
            return;
        }
        const float half = size * 0.5f;
        draw->AddImageRounded(TextureId(head), {center.x - half, center.y - half}, {center.x + half, center.y + half},
                              {0, 0}, {1, 1}, IM_COL32_WHITE, half);
        draw->AddCircle(center, half, Col(color::border), 0, Dp(1));
    }

    // ---------------------------------------------------------- competitive
    void StartCompetitive()
    {
        competitive_ = std::make_unique<CompetitiveService>(
            platform::ProfileSessionFile(DataFolder(base_), file_.path),
            platform::DiscordLinkFile(DataFolder(base_), file_.path));
        // The profile server trades the Discord SDK's tokens.
        discord_friends_ = std::make_unique<DiscordFriends>(
            base_, platform::DiscordLinkFile(DataFolder(base_), file_.path),
            [service = competitive_.get()](DiscordFriends::TradeRequest request,
                                           std::function<void(DiscordFriends::TradeResult)> done) {
                service->TradeDiscordToken(
                    std::move(request.code), std::move(request.verifier), std::move(request.redirect_uri),
                    std::move(request.refresh_token),
                    [done = std::move(done)](const platform::DiscordTokens& tokens, const std::string& error,
                                             bool refused) {
                        done({tokens.access_token, tokens.refresh_token, error, refused});
                    });
            });
    }

    // Every frame: the service follows online.profile_server (the settings
    // file's, the launcher never changes it); a new sign-in
    // opens its page in the browser, and a failure on the page is told.
    void UpdateCompetitive()
    {
        if (!competitive_) return;
        // What keeps this computer from playing a set (no game image, an
        // update it lacks...), for the buttons that would start one.
        set_blocker_ = SetBlocker();
        // The game this computer's sets play (the checked image's version).
        platform::ProfileService::SetPlayedGame(check_.info ? check_.info->game_id.substr(0, 4) : std::string());
        competitive_->SetServer(platform::ProfileServerAddress(draft_.profile_server).value_or(std::string()));
        competitive_status_ = competitive_->Snapshot();
        if (competitive_status_.state != CompetitiveService::State::SignedIn) signing_out_ = false;
        AdoptCodedMii();
        UpdateSignIns();
        UpdateRestore();
        UpdateProfileMaker();
        UpdateRenames();
        if (!capture_)
        {
            if (competitive_status_.state == CompetitiveService::State::SignedIn) RememberProfile();
            else if (competitive_status_.state == CompetitiveService::State::SignedOut) LookUpSaveMiis();
        }
        const auto& status = competitive_status_;
        if (status.sign_in != competitive_opened_)
        {
            competitive_opened_ = status.sign_in;
            if (!status.url.empty() && !capture_) SDL_OpenURL(status.url.c_str());
        }
        if (status.error_serial != competitive_errors_)
        {
            competitive_errors_ = status.error_serial;
            if (OnlinePage(page_) && !status.error.empty()) Notice(status.error, NoticeKind::Error);
        }
        // A challenge or set seen open that ends without a result: told once
        // as it ends.
        if (status.match && status.match->Open())
        {
            watched_match_ = status.match->id;
            watched_invite_ = status.match->state == "challenged";
        }
        else if (status.match && status.match->id == watched_match_)
        {
            watched_match_ = 0;
            const auto& match = *status.match;
            const std::string& them = match.opponent.name;
            const bool sent = match.side == "challenger";
            std::string text;
            if (match.state == "declined")
                text = sent ? texts::T("launcher.challenge.ended.declined", {{"name", them}}) : std::string();
            else if (match.state == "expired")
                text = sent ? texts::T("launcher.challenge.ended.no_answer", {{"name", them}})
                            : texts::T("launcher.challenge.ended.ran_out", {{"name", them}});
            else if (match.stage == "dissolved" || match.state == "dissolved")
                text = texts::T("launcher.challenge.ended.closed", {{"name", them}});
            else if (match.state == "cancelled")
                text = watched_invite_ && !sent ? texts::T("launcher.challenge.ended.withdrawn", {{"name", them}})
                                                : texts::T("launcher.challenge.ended.cancelled", {{"name", them}});
            if (!text.empty() && !capture_) Notice(text, NoticeKind::Info);
        }
        // A challenge to the player, and their challenge accepted (now their
        // Ready to play! is due): a chime, and the window flashes while it is
        // in the background. Once each.
        if (status.match && status.match->Open() && !capture_)
        {
            const auto& match = *status.match;
            const bool to_you = match.side == "challenged" && match.state == "challenged";
            // The set goes (the countdown starts); servers before sets: the
            // challenge accepted.
            const bool go = match.stage == "go"
                || (match.stage.empty() && match.side == "challenger" && match.state == "accepted");
            // (Not for a challenge accepted here: that Accept was just pressed.)
            const bool accepted_here = challenge_accepted_ == match.id;
            const std::int64_t alert = to_you ? match.id * 2 : go && !accepted_here ? GoKey(match) * 2 + 1 : 0;
            if (alert && alert != alerted_)
            {
                alerted_ = alert;
                Alert();
            }
        }
        UpdateSetPresence();
        UpdateDiscordFriends();
        // Both pressed Ready to play!: this launcher starts the set's game.
        // Both are ready: 3, 2, 1, then this launcher starts the set's game.
        // Each go once: a set that waited for both again and goes again
        // starts the games again.
        if (status.match && status.match->stage == "go" && GoKey(*status.match) != competitive_started_go_ && !capture_)
        {
            const std::int64_t key = GoKey(*status.match);
            const std::uint64_t now = SDL_GetTicks();
            if (countdown_go_ != key)
            {
                countdown_go_ = key;
                countdown_end_ = now + kSetCountdownMs;
            }
            if (now >= countdown_end_)
            {
                competitive_started_go_ = key;
                competitive_go_ = true;
            }
        }
        else if (!capture_)
            countdown_go_ = 0;
    }

    // A challenge's chime at the master volume (not when muted), and the
    // window flashes while it is in the background.
    void Alert()
    {
        if (!draft_.mute)
        {
            if (!alert_sound_) alert_sound_.emplace();
            alert_sound_->Play(float(draft_.master_volume) / 100.0f);
        }
        if (window_ && !(SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS))
            SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED);
    }

    // The Discord status while a set waits in the launcher ([discord]
    // rich_presence): against whom, and what for. Nothing otherwise: the
    // game shows its own, and a challenge to the player stays private until
    // they accept.
    void UpdateSetPresence()
    {
        if (capture_) return;
        const auto& status = competitive_status_;
        std::optional<platform::discord::Activity> activity;
        if (status.state == CompetitiveService::State::SignedIn && status.match && status.match->Open()
            && !(status.match->side == "challenged" && status.match->state == "challenged"))
        {
            const auto& match = *status.match;
            const std::string& them = match.opponent.name;
            activity.emplace();
            activity->details = texts::T("launcher.challenge.presence.set_against", {{"name", them}});
            activity->state = match.state == "challenged"
                ? texts::T("launcher.challenge.presence.waiting_accept", {{"name", them}})
                : match.stage == "go"     ? std::string(texts::T("launcher.challenge.presence.starting"))
                : !match.you_ready        ? std::string(texts::T("launcher.challenge.presence.getting_ready"))
                : !match.opponent_ready   ? texts::T("launcher.challenge.presence.waiting_ready", {{"name", them}})
                                          : std::string(texts::T("launcher.challenge.presence.starting"));
            activity->large_image = "logo";
            activity->large_text = "Recharged";
            activity->start = match.created_at;
        }
        if (!set_presence_)
        {
            if (!activity || !draft_.discord_status) return;
            const std::string application = !draft_.discord_application_id.empty()
                ? draft_.discord_application_id
                : std::string(mscharged::build::discord_application_id);
            if (application.empty()) return;
            platform::DiscordPresence::Options options;
            options.application_id = application;
            options.log = [](const std::string& text) { platform::SessionLogLine(text); };
            set_presence_.emplace(std::move(options));
        }
        set_presence_->Show(draft_.discord_status ? activity : std::nullopt);
    }

    // The Discord friends follow the sign-in and the server's Discord
    // application; their accounts go to the service, which finds those who
    // play Recharged.
    void UpdateDiscordFriends()
    {
        if (!discord_friends_) return;
        const auto& status = competitive_status_;
        DiscordFriends::Account account;
        if (status.state == CompetitiveService::State::SignedIn && !status.info.discord_application.empty()
            && !status.player.MiiAccount())
        {
            account.server = status.server;
            account.application = std::strtoull(status.info.discord_application.c_str(), nullptr, 10);
        }
        discord_friends_->SetAccount(account);
        discord_status_ = discord_friends_->Snapshot();
        // Development captures: RECHARGED_CAPTURE_DISCORD=preview shows made-up
        // Discord friends while signed in, and asks the server nothing.
        const char* preview = capture_ ? std::getenv("RECHARGED_CAPTURE_DISCORD") : nullptr;
        if (preview && std::string(preview).rfind("preview", 0) == 0)
        {
            PreviewDiscordFriends(preview);
            return;
        }
        std::vector<std::string> ids;
        if (discord_status_.state == DiscordFriends::State::Ready)
            for (const auto& entry : discord_status_.friends) ids.push_back(entry.id);
        if (ids != discord_asked_)
        {
            discord_asked_ = ids;
            competitive_->SetDiscordFriends(std::move(ids));
        }
        if (discord_status_.error_serial != discord_errors_)
        {
            discord_errors_ = discord_status_.error_serial;
            if (OnlinePage(page_) && !discord_status_.error.empty())
                Notice(discord_status_.error, discord_status_.state == DiscordFriends::State::NotLinked
                                                  ? NoticeKind::Info
                                                  : NoticeKind::Error);
        }
    }

    // Made-up online data for a development capture: a profile and Discord
    // friends, and with "preview-challenge..." a challenge.
    void PreviewDiscordFriends(const std::string& mode)
    {
        using Presence = DiscordFriends::Presence;
        if (mode.rfind("preview-welcome", 0) == 0 || mode.rfind("preview-mii", 0) == 0
            || mode.rfind("preview-profile", 0) == 0 || mode == "preview-discord-choice")
        {
            PreviewMiiAccount(mode);
            return;
        }
        if (competitive_status_.state != CompetitiveService::State::SignedIn)
        {
            // Signed out: a made-up profile on the server, for the pages that need one.
            competitive_status_.state = CompetitiveService::State::SignedIn;
            competitive_status_.player.id = 900000;
            competitive_status_.player.name = "Anosch";
            competitive_status_.player.discord = ".anosch";
            competitive_status_.player.discord_id = "80351110224678912";
            competitive_status_.player.account = "discord";
            competitive_status_.player.wfc = {{1000000000, "R4QE", "0868-9934-5920", "Anosch"}};
            competitive_status_.player.country = competitive_status_.player.country_detected = "CH";
            competitive_status_.player.country_choice = "auto";
            if (competitive_status_.info.version.empty()) competitive_status_.info.version = "1.2.1";
            if (competitive_status_.server.empty()) competitive_status_.server = draft_.profile_server;
            if (competitive_status_.rule_sets.empty())
            {
                platform::RuleSet set;
                set.id = 1;
                set.name = "Competitive";
                set.rules.skill = "rookie";
                set.rules.limit = "goals";
                set.rules.goals = 10;
                set.rules.games = 3;
                set.rules.stadium = "random";
                competitive_status_.rule_sets = {set};
            }
        }
        discord_status_.state = DiscordFriends::State::Ready;
        discord_status_.error.clear();
        discord_status_.friends = {{"900000000000000001", "Daisy", Presence::Idle, true},
                                   {"900000000000000002", "Peach", Presence::Online, true},
                                   {"900000000000000003", "Toad", Presence::Online, true},
                                   {"900000000000000004", "Luigi", Presence::Online, false},
                                   {"900000000000000005", "Wario", Presence::Idle, false},
                                   {"900000000000000006", "Bowser", Presence::Offline, false},
                                   {"900000000000000007", "Yoshi", Presence::DoNotDisturb, false},
                                   {"900000000000000008", "Waluigi", Presence::Offline, false}};
        const auto now = std::int64_t(std::time(nullptr));
        const auto make = [](std::int64_t id, const char* name, const char* discord, const char* discord_id) {
            platform::ProfilePlayer player;
            player.id = id;
            player.name = name;
            player.discord = discord;
            player.discord_id = discord_id;
            return player;
        };
        // The Discord friends who play Recharged (Player profile counts them).
        competitive_status_.discord_players = {make(900001, "Daisy", "daisy", "900000000000000001"),
                                               make(900002, "Peach", "peach", "900000000000000002"),
                                               make(900006, "Bowser", "bowser", "900000000000000006")};
        auto mario = make(900010, "Mario", "itsamario", "");
        mario.online = true;
        mario.activity = "launcher";
        // A challenge from Mario in the dialog: "preview-challenge", or
        // "preview-rematch" for a rematch; "preview-challenge-wait" after its
        // Accept while Mario takes his Ready to play! back, "preview-challenge-go"
        // its countdown.
        if (mode.rfind("preview-challenge", 0) == 0 || mode == "preview-rematch")
        {
            platform::ProfileMatch set;
            set.id = 77;
            set.state = "challenged";
            set.side = "challenged";
            set.stage = "invited";
            set.opponent = mario;
            set.rule_set = "Competitive";
            set.rules.skill = "rookie";
            set.rules.limit = "time";
            set.rules.time = 180;
            set.rules.games = 3;
            set.rules.stadium = "random";
            set.rules.tweaks = {"fast_terrain", "no_mega_strikes", "thunder_island_walls"};
            set.opponent_online = true;
            set.created_at = now;
            if (mode == "preview-rematch") set.rematch_of = 76;
            if (mode == "preview-challenge-wait" || mode == "preview-challenge-go")
            {
                set.state = "accepted";
                set.stage = mode == "preview-challenge-go" ? "go" : "open";
                set.you_ready = true;
                set.opponent_ready = mode == "preview-challenge-go";
                challenge_accepted_ = set.id;
            }
            if (mode == "preview-challenge-go")
            {
                set.go_at = now;
                countdown_go_ = set.go_at;
                countdown_end_ = SDL_GetTicks() + 2400; // a "3"
            }
            competitive_status_.match = set;
        }
    }

    // Made-up profile states for a development capture, offline:
    // "preview-welcome" (signed out: the ways to a profile; the save's Miis
    // that have been online show under Welcome back, the first remembered by
    // name), "preview-welcome-new" (none of them), "preview-profile-create" /
    // "-taken" (Create, its name free or taken), "preview-profile-import"
    // (Import's dialog), "preview-profile-finish" (a profile Discord just
    // made), "preview-mii-profile" / "-discord" / "-connect" / "-rename" /
    // "-signout" (a profile without Discord, with it, connecting it, its name
    // being changed, and Switch profile asked for one whose Mii has not been
    // online yet), "preview-discord-choice" (Discord had a profile of its
    // own), the Mii editor on the profile's Mii with "preview-mii-editor" /
    // "-<section>" ("-hair": another hairstyle chosen), and on Create's new
    // Mii with "preview-mii-new-editor".
    void PreviewMiiAccount(const std::string& mode)
    {
        auto& status = competitive_status_;
        status.info.discord = status.info.mii = true;
        status.info.version = "1.3.0";
        if (status.server.empty()) status.server = draft_.profile_server;
        status.error.clear();
        status.busy = false;
        const bool first = !preview_applied_;
        preview_applied_ = true;
        const auto& miis = InstalledMiis().miis;
        const bool editor = mode.rfind("preview-mii-editor", 0) == 0;
        const bool creating = mode.rfind("preview-profile-create", 0) == 0 || mode == "preview-mii-new-editor";
        if (mode.rfind("preview-welcome", 0) == 0 || creating || mode == "preview-profile-import")
        {
            status.state = CompetitiveService::State::SignedOut;
            status.known_miis.clear();
            status.known_asked.clear();
            if (mode != "preview-welcome-new")
            {
                std::vector<const platform::InstalledMii*> coded;
                for (const auto& mii : miis)
                    if (mii.friend_code) coded.push_back(&mii);
                // One of them plays for a profile, as on most computers.
                for (const auto* mii : coded) status.known_asked.push_back(std::uint32_t(mii->friend_code));
                if (!coded.empty()) status.known_miis.emplace_back(std::uint32_t(coded.front()->friend_code), "game");
                if (first && !coded.empty())
                {
                    known_profiles_read_ = true;
                    known_profiles_ = {{status.server, 900020, "Bob the Builder", "mii",
                                        {platform::FriendCodeText(coded.front()->friend_code)}}};
                }
            }
            if (creating)
            {
                status.checked_name = "Bob the Builder";
                status.name_free = mode != "preview-profile-create-taken";
            }
            if (!first) return;
            if (mode == "preview-profile-import") profile_import_prompt_ = true;
            if (creating)
            {
                OpenProfileMaker(false);
                maker_.color = 5;
                RollMakerFace(7);
                maker_.name = maker_.asked_name = "Bob the Builder";
                maker_.typed_at = 1;
            }
            if (mode == "preview-mii-new-editor")
            {
                OpenMiiEditor(true, MakerNewRecord(maker_.created), false, true);
                mii_editor_.page = PageOnlineSettings;
            }
            return;
        }
        status.state = CompetitiveService::State::SignedIn;
        status.player = {};
        status.player.id = 900020;
        status.player.name = "Bob the Builder";
        status.player.account = "mii";
        status.player.friend_code = "0868-9934-5920";
        status.player.wfc = {{1000000000, "R4QE", "0868-9934-5920", miis.empty() ? "Bob" : miis.front().name}};
        if (mode == "preview-profile-finish")
        {
            status.player.account = "discord";
            status.player.discord = "bob";
            status.player.friend_code.clear();
            status.player.wfc.clear();
            if (first)
            {
                OpenProfileMaker(true);
                maker_.color = 5;
                RollMakerFace(7);
            }
            return;
        }
        if (mode == "preview-mii-profile-discord")
        {
            status.player.account = "discord";
            status.player.discord = "bob";
        }
        if (mode == "preview-discord-choice")
        {
            platform::ProfilePlayer other;
            other.id = 900021;
            other.name = "Bob";
            other.account = "discord";
            other.discord = "bob";
            status.discord_choice = CompetitiveService::Status::DiscordChoice{other, "preview"};
        }
        if (mode == "preview-mii-profile-connect")
        {
            status.connecting_discord = true;
            status.code = "K7Q2";
            status.url = "https://example.invalid/sign-in";
        }
        if (mode == "preview-mii-profile-rename" && first)
        {
            profile_renaming_ = true;
            rename_text_ = "Bob B.";
        }
        if (mode == "preview-mii-profile-signout")
        {
            status.player.friend_code.clear();
            status.player.wfc.clear();
            if (first) sign_out_prompt_ = true;
        }
        if (first && !miis.empty()) draft_.online_mii = platform::MiiCreateIdText(miis.front().create_id);
        if (first && editor)
        {
            // The player's Mii, else a made-up one.
            const std::string record =
                !miis.empty() ? miis.front().record : platform::MakeRandomMii("Bob", 7, 1760000000, 5);
            OpenMiiEditor(false, record, miis.empty(), true);
            mii_editor_.page = PageOnlineSettings;
            // "-<section>" shows that section ("-facial" Facial hair, "-name" Name & colour).
            const std::string wanted = mode.size() > 19 ? mode.substr(19) : std::string();
            const auto& sections = MiiEditorSections();
            for (std::size_t n = 0; n < sections.size() && !wanted.empty(); ++n)
                if (std::string_view(sections[n].key).rfind(wanted, 0) == 0) mii_editor_.section = int(n);
            if (mode == "preview-mii-editor-hair")
            {
                mii_editor_.section = 1;
                mii_editor_.undo.push_back({mii_editor_.face, mii_editor_.name});
                mii_editor_.face.hair_type = 33;
                mii_editor_.face.hair_color = 7;
            }
        }
    }

    // The player's own Mii as the Online pages show it.
    struct OwnMii
    {
        std::string name;
        int color = 0;
        std::array<unsigned char, 8> id{};
        std::string record;            // empty while it isn't among this computer's Miis
        std::uint64_t friend_code = 0; // from the game's online save, once it logged in
        bool here = false;             // among this computer's Miis
    };

    // Whether the player's sets go with a Mii they chose (online.mii): a Mii
    // player, or one who connected Discord to their Mii profile here.
    // Otherwise a Discord (or test) player's Mii is made from the account.
    bool ChosenOwnMii() const
    {
        return !draft_.online_mii.empty() || competitive_status_.player.MiiAccount();
    }

    // The player's own Mii while signed in: the chosen one (nullopt when none
    // is chosen), else the one made from the account, whether or not it has
    // been made in the save folder yet.
    // A Mii player's Mii is the one with the profile's friend code (found
    // again on any computer, after Sign in through the game too): kept as
    // online.mii once it is found. Once a frame, not from the drawing.
    void AdoptCodedMii()
    {
        const auto& status = competitive_status_;
        if (status.state != CompetitiveService::State::SignedIn || !ChosenOwnMii() || !status.player.MiiAccount())
            return;
        const auto id = platform::ParseMiiCreateId(draft_.online_mii);
        if (const auto coded = ProfileCodedMii(id); coded && coded != id)
            ChooseOnlineMii(platform::MiiCreateIdText(*coded));
    }

    std::optional<OwnMii> PlayerOwnMii()
    {
        const auto& status = competitive_status_;
        if (status.state != CompetitiveService::State::SignedIn) return std::nullopt;
        OwnMii own;
        if (ChosenOwnMii())
        {
            // A profile has one Mii. A Mii player's is the Mii with the
            // profile's friend code, found again on any computer it is on
            // (after Sign in through the game, too), and kept as online.mii;
            // before its first online game, the Mii chosen with the profile.
            // (AdoptCodedMii keeps it as online.mii; this only reads.)
            auto id = platform::ParseMiiCreateId(draft_.online_mii);
            if (status.player.MiiAccount())
                if (const auto coded = ProfileCodedMii(id)) id = coded;
            if (!id) return std::nullopt;
            own.id = *id;
        }
        else
        {
            const std::string key = PlayerMiiKey();
            const auto& player = status.player;
            own.name = platform::FitMiiName(player.name, player.discord);
            if (keyed_mii_for_ != key + '\n' + own.name)
            {
                keyed_mii_for_ = key + '\n' + own.name;
                keyed_mii_record_ = platform::MakeKeyedMii(own.name, key);
            }
            own.id = platform::KeyedMiiCreateId(key);
            own.record = keyed_mii_record_;
            own.color = platform::MiiRecordFavoriteColor(own.record);
        }
        for (const auto& mii : InstalledMiis().miis)
            if (mii.create_id == own.id)
            {
                own.name = mii.name;
                own.color = mii.favorite_color;
                own.record = mii.record;
                own.friend_code = mii.friend_code;
                own.here = true;
            }
        return own;
    }

    // This computer's Mii with one of the profile's friend codes, preferring
    // `chosen`: nullopt when there is none, or when `chosen` is here and has
    // no friend code yet (its first online game gives it the profile's).
    std::optional<std::array<unsigned char, 8>> ProfileCodedMii(
        const std::optional<std::array<unsigned char, 8>>& chosen)
    {
        const auto digits = [](std::string_view text) {
            std::string out;
            for (const char c : text)
                if (c >= '0' && c <= '9') out += c;
            return out;
        };
        const auto& player = competitive_status_.player;
        std::vector<std::string> codes;
        if (!player.friend_code.empty()) codes.push_back(digits(player.friend_code));
        for (const auto& profile : player.wfc) codes.push_back(digits(profile.friend_code));
        const auto profiles = [&](const platform::InstalledMii& mii) {
            return mii.friend_code
                && std::find(codes.begin(), codes.end(), digits(platform::FriendCodeText(mii.friend_code)))
                != codes.end();
        };
        std::optional<std::array<unsigned char, 8>> found;
        for (const auto& mii : InstalledMiis().miis)
        {
            if (chosen && mii.create_id == *chosen && (!mii.friend_code || profiles(mii))) return std::nullopt;
            if (!found && profiles(mii)) found = mii.create_id;
        }
        return found;
    }

    // The key of the player's own Mii (made from their Discord account, the
    // same on every computer); empty when not signed in, and for a player
    // whose sets go with a Mii they chose.
    std::string PlayerMiiKey() const
    {
        const auto& status = competitive_status_;
        if (status.state != CompetitiveService::State::SignedIn || signing_out_ || ChosenOwnMii()) return {};
        const auto& player = status.player;
        return !player.discord_id.empty() ? "discord:" + player.discord_id
                                          : "player:" + status.server + "#" + std::to_string(player.id);
    }

    // The player's own Mii (made from their Discord account, the same on
    // every computer) in the save folder, added or renamed only when needed:
    // its key, or empty when not signed in. Throws when it can't be made.
    std::string EnsurePlayerMii()
    {
        const std::string key = PlayerMiiKey();
        if (key.empty()) return key;
        const auto& player = competitive_status_.player;
        const auto folder = SaveFolder();
        mscharged::platform::EnsureMii(
            mscharged::platform::MakeKeyedMii(mscharged::platform::FitMiiName(player.name, player.discord), key),
            folder, SaveTitleOf(Release()), folder / "backups");
        ForgetInstalledMiis();
        return key;
    }

    // A set's go: the player's own Mii (chosen, or EnsurePlayerMii), then the game,
    // which goes through the online login straight into RANKED against the
    // opponent. Anything in the way takes the player's Ready to play! back.
    void StartSet(bool& quit)
    {
        const auto& status = competitive_status_;
        if (!status.match) return;
        if (const auto blocker = SetBlocker(); !blocker.empty())
        {
            set_start_error_ = blocker;
            Notice(texts::T("launcher.challenge.cannot_start", {{"reason", blocker}}), NoticeKind::Error);
            competitive_->ReadyToPlay(status.match->id, false);
            return;
        }
        // The player's own Mii: the chosen one, which must be here, or the
        // one made from the account.
        std::array<unsigned char, 8> mii{};
        if (ChosenOwnMii())
        {
            const auto own = PlayerOwnMii();
            if (!own || !own->here)
            {
                set_start_error_ = texts::T("launcher.challenge.choose_mii");
                Notice(texts::T("launcher.challenge.cannot_start", {{"reason", set_start_error_}}), NoticeKind::Error);
                competitive_->ReadyToPlay(status.match->id, false);
                return;
            }
            mii = own->id;
        }
        else
        {
            try
            {
                mii = mscharged::platform::KeyedMiiCreateId(EnsurePlayerMii());
            }
            catch (const std::exception& e)
            {
                set_start_error_ = texts::T("launcher.challenge.mii_failed", {{"error", e.what()}});
                Notice(set_start_error_, NoticeKind::Error);
                competitive_->ReadyToPlay(status.match->id, false);
                return;
            }
        }
        // The set's go leaves no moment to ask about unsaved changes, and the
        // launcher closes: they are saved (as Play's Save and play).
        if (dirty_ && config_ok_) Save();
        frontend_launch_ = GameLaunch();
        frontend_launch_->start_competitive = true;
        frontend_launch_->competitive_set = status.match->id;
        frontend_launch_->competitive_mii = mii;
        // The game and the launcher it opens again afterwards see it.
        SetEnvironment(kCompetitiveSetVariable, std::to_string(GoKey(*status.match)));
        quit = true;
    }

    // What the challenge dialog shows: nothing, a challenge to answer, the
    // Accept on its way, an accepted set that waits (for the opponent's
    // Ready to play!, or after its game could not start), or the countdown
    // to the set's game.
    enum class ChallengeView { None, Invite, Accepting, Waiting, Starting };

    ChallengeView CurrentChallengeView() const
    {
        const auto& status = competitive_status_;
        if (status.state != CompetitiveService::State::SignedIn || !status.match || !status.match->Open())
            return ChallengeView::None;
        const auto& match = *status.match;
        if (match.stage == "go" && countdown_go_ == GoKey(match) && competitive_started_go_ != countdown_go_)
            return ChallengeView::Starting;
        if (match.state == "challenged" && match.side == "challenged")
        {
            if (challenge_declined_ == match.id) return ChallengeView::None;
            return challenge_accepted_ == match.id && status.busy ? ChallengeView::Accepting : ChallengeView::Invite;
        }
        if (challenge_accepted_ == match.id && match.state == "accepted" && match.stage != "playing")
            return ChallengeView::Waiting;
        return ChallengeView::None;
    }

    // A challenge to the player (or a rematch they are asked for), over any
    // page as a card over the dimmed launcher: Accept starts the game
    // straight into the set, Decline (Escape, B) answers no. After Accept it
    // stays until the game starts. It opens when no other dialog is open,
    // and once answered no it does not come back for that challenge.
    void DrawChallengeDialog()
    {
        const ChallengeView view = CurrentChallengeView();
        if (view != ChallengeView::None
            && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::OpenPopup("Challenge");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(Dp(560), viewport->Size.x - Dp(48));
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({width, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(30, 26));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0, 0, 0, 0});
        if (ImGui::BeginPopupModal("Challenge", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Dp(10), 0.0f});
            if (view == ChallengeView::None) ImGui::CloseCurrentPopup();
            else DrawChallengeContents(view, *competitive_status_.match);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void DrawChallengeContents(ChallengeView view, const platform::ProfileMatch& match)
    {
        const auto& fonts = CurrentFonts();
        const auto& status = competitive_status_;
        const bool asked = view == ChallengeView::Invite || view == ChallengeView::Accepting;
        const bool rematch = match.rematch_of != 0;
        const ImVec4 tint = color::accent;
        auto* draw = ImGui::GetWindowDrawList();
        DrawDialogCard(tint);
        const float right_edge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;

        // Header: a trophy in a soft halo, what it is, who, and the rules.
        const std::string& them = match.opponent.name;
        const char* overline = rematch ? texts::T("launcher.challenge.overline.rematch")
            : asked ? (match.random ? texts::T("launcher.challenge.overline.random")
                                    : texts::T("launcher.challenge.overline.challenge"))
                    : texts::T("launcher.challenge.overline.set");
        const std::string headline = !asked ? texts::T("launcher.challenge.your_set", {{"name", them}})
            : rematch                       ? texts::T("launcher.challenge.wants_rematch", {{"name", them}})
                                            : texts::T("launcher.challenge.challenges_you", {{"name", them}});
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float disc = Dp(46);
        DialogBadge(draw, {top.x + disc * 0.5f, top.y + Dp(2) + disc * 0.5f}, disc, Icon::Trophy, tint);
        ImGui::SetCursorScreenPos({top.x + disc + Dp(20), top.y});
        ImGui::BeginGroup();
        DrawTracked(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(tint, 0.95f), overline, Dp(1.4f));
        ImGui::Dummy({0, FontSize(fonts.caption) + Dp(5)});
        TextColored(fonts.heading, color::text, headline.c_str());
        if (match.opponent.MiiAccount())
        {
            // A Mii player: their name with their friend code.
            const std::string code = !match.opponent.friend_code.empty()
                ? match.opponent.friend_code
                : std::string(texts::T("launcher.challenge.no_friend_code"));
            TextColored(fonts.caption, color::muted, (them + "  ·  " + code).c_str());
        }
        ImGui::Dummy({0, Dp(3)});
        TextWrappedColored(fonts.body, Mix(color::muted, color::text, 0.45f),
                           match.rule_set.empty() ? texts::T("launcher.challenge.a_set") : match.rule_set.c_str());
        ImGui::Dummy({0, Dp(2)});
        TextWrappedColored(fonts.caption, color::muted, (platform::DescribeRules(match.rules) + ".").c_str());
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos({top.x, std::max(top.y + disc + Dp(4), ImGui::GetItemRectMax().y)});
        ImGui::Dummy({0, Dp(18)});

        // What happens, or what holds it up.
        const bool blocked = !set_blocker_.empty();
        if (view == ChallengeView::Invite)
        {
            if (blocked)
                Callout(texts::T("launcher.challenge.blocked", {{"reason", set_blocker_}}), color::warning,
                        Icon::Warning);
            else TextWrappedColored(fonts.body, color::text, texts::T("launcher.challenge.accept_about"));
            if (challenge_accepted_ == match.id && status.error_serial != challenge_errors_ && !status.error.empty())
            {
                ImGui::Dummy({0, Dp(10)});
                Callout(status.error, color::danger, Icon::Error);
            }
            ImGui::Dummy({0, Dp(18)});
        }
        else if (view == ChallengeView::Waiting && !match.you_ready)
        {
            Callout(set_start_error_.empty()
                        ? std::string(texts::T("launcher.challenge.start_failed"))
                        : texts::T("launcher.challenge.start_failed_why", {{"reason", set_start_error_}}),
                    color::warning, Icon::Warning);
            ImGui::Dummy({0, Dp(18)});
        }
        else if (view == ChallengeView::Waiting && !match.opponent_online)
        {
            Callout(texts::T("launcher.challenge.opponent_closed", {{"name", them}}), color::warning, Icon::Warning);
            ImGui::Dummy({0, Dp(18)});
        }

        // Footer: the state at the left, the actions at the right.
        DialogDivider();
        ImGui::Dummy({0, Dp(18)});
        const float row = ImGui::GetCursorScreenPos().y, height = Dp(40);
        const auto align_right = [&](float width) { ImGui::SetCursorScreenPos({right_edge - width, row}); };
        const auto state_line = [&](const std::string& text) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            Spinner({pos.x + Dp(8), pos.y + height * 0.5f}, Dp(7), Col(tint));
            DrawLabel(draw, fonts.body, {pos.x + Dp(26), pos.y + (height - FontSize(fonts.body)) * 0.5f},
                      Col(color::text), text.c_str());
        };
        const bool busy = status.busy;
        if (view == ChallengeView::Invite)
        {
            const char *decline = texts::T("launcher.challenge.decline"),
                       *accept = texts::T("launcher.challenge.accept");
            const float decline_width = GhostWidth(decline, Icon::Close);
            const float accept_width = blocked ? 0.0f : PrimaryWidth(accept, Icon::Play);
            align_right(decline_width + (blocked ? 0.0f : Dp(10) + accept_width));
            if (GhostButton("challenge_decline", decline, {decline_width, height}, !busy, Icon::Close)
                || (CancelPressed() && !busy))
            {
                challenge_declined_ = match.id;
                competitive_->Decline(match.id);
            }
            // The answer in focus, with its cursor shown, so the first Return
            // or A answers (without the cursor ImGui spends it on showing it).
            const auto focus = [] {
                ImGui::SetItemDefaultFocus();
                if (ImGui::IsWindowAppearing()) ImGui::SetNavCursorVisible(true);
            };
            if (blocked) focus();
            else
            {
                ImGui::SameLine(0, Dp(10));
                if (PrimaryButton("challenge_accept", accept, {accept_width, height}, !busy, Icon::Play))
                {
                    challenge_accepted_ = match.id;
                    challenge_errors_ = status.error_serial;
                    set_start_error_.clear();
                    competitive_->Accept(match.id);
                }
                focus();
            }
        }
        else if (view == ChallengeView::Accepting) state_line(texts::T("launcher.challenge.accepting"));
        else if (view == ChallengeView::Starting)
        {
            const std::uint64_t now = SDL_GetTicks();
            const int seconds = int(((countdown_end_ > now ? countdown_end_ - now : 0) + 999) / 1000);
            state_line(seconds > 0 ? texts::T("launcher.challenge.starting_in", {{"seconds", std::to_string(seconds)}})
                                   : std::string(texts::T("launcher.challenge.starting")));
        }
        else
        {
            const char *cancel = texts::T("launcher.challenge.cancel_set"),
                       *again = texts::T("launcher.challenge.try_again");
            const bool retry = !match.you_ready && !blocked;
            if (match.you_ready) state_line(texts::T("launcher.challenge.waiting_ready", {{"name", them}}));
            const float cancel_width = GhostWidth(cancel, Icon::Close);
            const float again_width = retry ? PrimaryWidth(again, Icon::Play) : 0.0f;
            align_right(cancel_width + (retry ? Dp(10) + again_width : 0.0f));
            if (GhostButton("challenge_cancel", cancel, {cancel_width, height}, !busy, Icon::Close))
                competitive_->CancelMatch(match.id);
            if (retry)
            {
                ImGui::SameLine(0, Dp(10));
                if (PrimaryButton("challenge_again", again, {again_width, height}, !busy, Icon::Play))
                {
                    set_start_error_.clear();
                    competitive_->ReadyToPlay(match.id, true);
                }
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, row});
        ImGui::Dummy({1.0f, height});
    }

    // Nothing on its way from the profile service or Discord (for
    // development captures).
    bool CompetitiveSettled() const
    {
        if (!competitive_) return true;
        const auto& status = competitive_status_;
        const bool discord_due = status.state == CompetitiveService::State::SignedIn
            && !status.info.discord_application.empty();
        const auto discord = discord_status_.state;
        const bool discord_settled = !discord_friends_ || discord == DiscordFriends::State::Missing
            || (discord != DiscordFriends::State::Connecting && (discord != DiscordFriends::State::Off || !discord_due));
        return status.state != CompetitiveService::State::Connecting && !status.busy && discord_settled;
    }

    // ------------------------------------------- profiles of this computer
    // A profile that signed in here is remembered (KnownProfile), so that
    // after signing out the launcher can offer it again by name for a Mii of
    // the game save that plays for it. Signed out, the server tells for which
    // of those Miis a profile exists and how it signs in again (LookUpMiis):
    // a new profile is not made for a Mii that has one, which would split a
    // player in two (on the leaderboard, too).
    const std::vector<platform::KnownProfile>& KnownProfiles()
    {
        if (!known_profiles_read_)
        {
            known_profiles_read_ = true;
            known_profiles_ = platform::LoadKnownProfiles(platform::KnownProfilesFile(DataFolder(base_), file_.path));
        }
        return known_profiles_;
    }

    static std::string FriendCodeDigits(std::string_view text)
    {
        std::string out;
        for (const char c : text)
            if (c >= '0' && c <= '9') out += c;
        return out;
    }

    // The signed-in profile, kept first in the remembered ones when it changes.
    void RememberProfile()
    {
        const auto& status = competitive_status_;
        if (status.serial == known_profiles_serial_) return;
        known_profiles_serial_ = status.serial;
        const auto& player = status.player;
        if (player.id <= 0 || status.server.empty()) return;
        platform::KnownProfile profile;
        profile.server = status.server;
        profile.player_id = player.id;
        profile.name = player.name;
        profile.account = player.account;
        const auto add = [&](const std::string& code) {
            if (!code.empty() && std::find(profile.friend_codes.begin(), profile.friend_codes.end(), code)
                                     == profile.friend_codes.end())
                profile.friend_codes.push_back(code);
        };
        add(player.friend_code);
        for (const auto& wfc : player.wfc) add(wfc.friend_code);
        auto profiles = KnownProfiles();
        const auto same = [&](const platform::KnownProfile& known) {
            return known.server == profile.server && known.player_id == profile.player_id;
        };
        if (!profiles.empty() && same(profiles.front()) && profiles.front().name == profile.name
            && profiles.front().account == profile.account && profiles.front().friend_codes == profile.friend_codes)
            return;
        profiles.erase(std::remove_if(profiles.begin(), profiles.end(), same), profiles.end());
        profiles.insert(profiles.begin(), std::move(profile));
        if (profiles.size() > 8) profiles.resize(8);
        try
        {
            platform::SaveKnownProfiles(platform::KnownProfilesFile(DataFolder(base_), file_.path), profiles);
            known_profiles_ = std::move(profiles);
        }
        catch (const std::exception& e)
        {
            platform::SessionLogLine(std::string("Online: the profiles of this computer are not kept: ") + e.what());
            known_profiles_ = std::move(profiles);
        }
    }

    // Signed out: the server is asked which of the game save's Miis play for
    // a profile (once for each list of them).
    void LookUpSaveMiis()
    {
        std::vector<std::uint32_t> ids;
        for (const auto& mii : InstalledMiis().miis)
            if (mii.friend_code && ids.size() < 32) ids.push_back(std::uint32_t(mii.friend_code));
        std::sort(ids.begin(), ids.end());
        competitive_->LookUpMiis(std::move(ids));
    }

    // The profile a Mii of the game save plays for, when one does: how it
    // signs in again ("game": that Mii's WFC login; "discord") and its name
    // when it signed in here. The server's answer counts; without one (an
    // older server, or none yet) what this computer remembers.
    struct MiiProfile
    {
        std::string sign_in;
        std::string name;
    };
    std::optional<MiiProfile> ProfileOfMii(const platform::InstalledMii& mii)
    {
        if (!mii.friend_code) return std::nullopt;
        const auto& status = competitive_status_;
        const std::string code = FriendCodeDigits(platform::FriendCodeText(mii.friend_code));
        MiiProfile profile;
        std::string remembered;
        for (const auto& known : KnownProfiles())
        {
            if (known.server != status.server) continue;
            for (const auto& own : known.friend_codes)
                if (FriendCodeDigits(own) == code && remembered.empty())
                {
                    profile.name = known.name;
                    remembered = known.account == "mii" ? "game" : known.account;
                }
        }
        const auto id = std::uint32_t(mii.friend_code);
        if (std::find(status.known_asked.begin(), status.known_asked.end(), id) != status.known_asked.end())
        {
            for (const auto& [known_id, sign_in] : status.known_miis)
                if (known_id == id) profile.sign_in = sign_in;
        }
        else profile.sign_in = remembered;
        if (profile.sign_in != "game" && profile.sign_in != "discord") return std::nullopt;
        return profile;
    }

    // ------------------------------------------------- Online player profile
    // Signed out, the page offers the ways to a profile, top to bottom: the
    // profiles this computer's game save brings back (Continue), Connect
    // Discord (a profile on any computer) and Create a profile, then Import
    // (an old save's Miis that have been online). Signing in shows its code;
    // what keeps the server from being used shows instead.
    void OnlineWelcome()
    {
        using State = CompetitiveService::State;
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        PageHeader(texts::T("match_setup.profile_page.title"), texts::T("match_setup.profile_page.about"));
        switch (status.state)
        {
        case State::SignedOut:
            WelcomeBackCard();
            ProfileChoices();
            break;
        case State::SigningIn:
        {
            BeginCard("##online_signing_in", nullptr);
            const float width = ImGui::GetContentRegionAvail().x, left = ImGui::GetCursorPosX();
            const auto centred = [&](ImFont* font, const ImVec4& tint, const std::string& text) {
                ImGui::SetCursorPosX(left + std::max(0.0f, (width - TextSize(font, text.c_str()).x) * 0.5f));
                TextColored(font, tint, text.c_str());
            };
            ImGui::Dummy({0, Dp(6)});
            centred(fonts.heading, color::text, texts::T("match_setup.welcome.finish_in_browser"));
            centred(fonts.body, color::muted, texts::T("match_setup.welcome.check_code"));
            ImGui::Dummy({0, Dp(4)});
            centred(fonts.title, color::accent, status.code);
            ImGui::Dummy({0, Dp(10)});
            const float buttons = Dp(210) + Dp(10) + Dp(120);
            ImGui::SetCursorPosX(left + (width - buttons) * 0.5f);
            if (SecondaryButton("sign_in_again", texts::T("match_setup.welcome.open_again"), Dp(210, 40),
                                !status.url.empty(), Icon::Link))
                SDL_OpenURL(status.url.c_str());
            ImGui::SameLine(0, Dp(10));
            if (GhostButton("sign_in_stop", texts::T("match_setup.buttons.cancel"), Dp(120, 40)))
                competitive_->StopSignIn();
            ImGui::Dummy({0, Dp(6)});
            EndCard();
            break;
        }
        case State::Connecting:
        {
            BeginCard("##online_connecting", nullptr);
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            Spinner({at.x + Dp(10), at.y + Dp(12)}, Dp(8), Col(color::muted));
            DrawLabel(draw, fonts.body, {at.x + Dp(28), at.y + Dp(12) - FontSize(fonts.body) * 0.5f}, Col(color::muted),
                      texts::T("match_setup.welcome.connecting"));
            ImGui::Dummy({ImGui::GetContentRegionAvail().x, Dp(24)});
            EndCard();
            break;
        }
        case State::NoServer:
            Callout(texts::T("match_setup.welcome.no_server",
                             {{"key", "online.profile_server"}, {"file", PathUtf8(file_.path.filename())}}),
                    color::warning, Icon::Warning);
            break;
        default:
            Callout(status.error.empty()
                        ? std::string(texts::T("match_setup.welcome.no_answer"))
                        : texts::T("match_setup.welcome.error_retry",
                                   {{"error", status.error + (status.error.back() != '.' ? "." : "")}}),
                    color::warning, Icon::Warning);
            break;
        }

        // The server, for those who run their own.
        std::string host = status.server.empty() ? draft_.profile_server : status.server;
        if (const auto scheme = host.find("://"); scheme != std::string::npos) host.erase(0, scheme + 3);
        const std::string server = texts::T("match_setup.welcome.server", {{"host", host}})
            + (status.info.version.empty() ? std::string() : "  ·  " + status.info.version);
        ImGui::Dummy({0, Dp(2)});
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                             + std::max(0.0f, (width - TextSize(fonts.caption, server.c_str()).x) * 0.5f));
        TextColored(fonts.caption, color::dim, server.c_str());
        if (status.state == State::SignedOut && status.info.test) CompetitiveTestSignIn();
    }

    // The profiles this computer's game save brings back: its Miis that play
    // for one (ProfileOfMii), each with Continue. Its online slot signs in
    // (servers from 1.3.0), as the game's own WFC login would, without the game.
    void WelcomeBackCard()
    {
        std::vector<std::pair<const platform::InstalledMii*, MiiProfile>> back;
        for (const auto& mii : InstalledMiis().miis)
            if (const auto profile = ProfileOfMii(mii)) back.emplace_back(&mii, *profile);
        if (back.empty()) return;
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        BeginCard("##welcome_back", texts::T("match_setup.profile_page.back.title"),
                  texts::T("match_setup.profile_page.back.about"), Icon::Person);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x, height = Dp(58), figure = Dp(42), button = Dp(160);
        // A few at first: a family's computer may have many.
        constexpr std::size_t kShown = 3;
        const std::size_t shown = show_all_back_ ? back.size() : std::min(back.size(), kShown);
        for (std::size_t n = 0; n < shown; ++n)
        {
            const auto& [mii, profile] = back[n];
            ImGui::PushID(int(n));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            draw->AddRectFilled(at, {at.x + width, at.y + height}, Col(color::field), Dp(12));
            draw->AddRect(at, {at.x + width, at.y + height}, Col(color::border, 0.8f), Dp(12));
            DrawMii(draw, {at.x + Dp(12) + figure * 0.5f, at.y + height * 0.5f}, figure, mii->favorite_color,
                    mii->record);
            const float left = at.x + Dp(12) + figure + Dp(14), room = at.x + width - button - Dp(24) - left;
            const float block = FontSize(fonts.label) + Dp(2) + FontSize(fonts.caption);
            const float top = at.y + (height - block) * 0.5f;
            DrawClipped(draw, fonts.label, {left, top}, room, color::text, profile.name.empty() ? mii->name : profile.name);
            DrawClipped(draw, fonts.caption, {left, top + FontSize(fonts.label) + Dp(2)}, room, color::muted,
                        texts::T("match_setup.profile_page.back.mii",
                                 {{"mii", mii->name}, {"code", platform::FriendCodeText(mii->friend_code)}}));
            ImGui::SetCursorScreenPos({at.x + width - button - Dp(12), at.y + (height - Dp(40)) * 0.5f});
            if (profile.sign_in == "discord")
            {
                // A server before 1.3.0: Discord brings that profile back.
                if (status.info.discord && DiscordButton("back_discord", texts::T("match_setup.discord.connect"),
                                                         {button, Dp(40)})
                    && !status.busy)
                    competitive_->SignIn();
            }
            else
            {
                const auto slot = platform::ReadOnlineSlot(mii->slot);
                const bool can = status.info.Profiles() && slot && !status.busy;
                const char* label = texts::T("match_setup.profile_page.back.continue");
                if (n == 0 ? PrimaryButton("back_continue", label, {button, Dp(40)}, can, Icon::Play, can)
                           : SecondaryButton("back_continue", label, {button, Dp(40)}, can, Icon::Play))
                {
                    continuing_ = mii->create_id;
                    competitive_->SaveSignIn(slot->user_id, slot->gsbrcd);
                }
                if (!status.info.Profiles() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", texts::T("match_setup.profile_page.back.old_server"));
            }
            ImGui::SetCursorScreenPos(at);
            ImGui::Dummy({width, height});
            if (n + 1 < shown) ImGui::Dummy({0, Dp(4)});
            ImGui::PopID();
        }
        if (shown < back.size())
        {
            ImGui::Dummy({0, Dp(4)});
            const std::string more = texts::Count("match_setup.profile_page.back.more", back.size() - shown);
            if (GhostButton("back_more", more.c_str(), {GhostWidth(more.c_str(), Icon::None), Dp(32)}))
                show_all_back_ = true;
        }
        if (continuing_ && !status.mii_error.empty())
        {
            ImGui::Dummy({0, Dp(8)});
            Callout(status.mii_error + (status.mii_error.back() == '.' ? "" : "."), color::danger, Icon::Error);
        }
        EndCard();
    }

    // The two ways to a profile side by side: Connect Discord (the profile on
    // any computer: a known Discord account brings its profile back) and
    // Create a profile (a name and a face, no account), then Import.
    void ProfileChoices()
    {
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        struct Choice
        {
            const char* title;
            const char* text;
            ImVec4 tint;
        };
        const Choice choices[] = {
            {texts::T("match_setup.discord.connect"), texts::T("match_setup.profile_page.discord_about"),
             color::discord},
            {texts::T("match_setup.profile_page.create.title"), texts::T("match_setup.profile_page.create.about"),
             color::accent},
        };
        const float width = ImGui::GetContentRegionAvail().x;
        const float gap = Dp(14), tile = (width - gap) * 0.5f, pad = Dp(18), badge = Dp(38), button = Dp(44);
        const float text_width = tile - pad * 2;
        float text_height = 0.0f;
        for (const auto& choice : choices)
            text_height = std::max(text_height, fonts.body->CalcTextSizeA(FontSize(fonts.body), FLT_MAX, text_width,
                                                                          choice.text).y);
        // The badge beside the title, the text under both, the button at the bottom.
        const float height = pad + badge + Dp(10) + text_height + Dp(14) + button + pad;
        const ImVec2 top = ImGui::GetCursorScreenPos();
        for (int n = 0; n < 2; ++n)
        {
            const Choice& choice = choices[n];
            const ImVec2 at{top.x + n * (tile + gap), top.y};
            const ImVec2 end{at.x + tile, at.y + height};
            FillGradient(draw, at, end, Col(Mix(color::card, choice.tint, 0.08f)), Col(color::card), Dp(16), false);
            draw->AddRect(at, end, Col(Mix(color::border, choice.tint, 0.35f), 0.9f), Dp(16));
            const ImVec2 icon{at.x + pad + badge * 0.5f, at.y + pad + badge * 0.5f};
            draw->AddRectFilled({icon.x - badge * 0.5f, icon.y - badge * 0.5f},
                                {icon.x + badge * 0.5f, icon.y + badge * 0.5f}, Col(choice.tint, n == 0 ? 1.0f : 0.16f),
                                Dp(12));
            if (n == 0) DrawDiscordMark(draw, icon, Dp(22), IM_COL32_WHITE, Col(choice.tint));
            else DrawIcon(draw, Icon::Person, icon, Dp(19), Col(choice.tint));
            DrawLabel(draw, fonts.heading, {at.x + pad + badge + Dp(14), icon.y - FontSize(fonts.heading) * 0.5f},
                      Col(color::text), choice.title);
            draw->AddText(fonts.body, FontSize(fonts.body), {at.x + pad, at.y + pad + badge + Dp(10)},
                          Col(color::muted), choice.text, nullptr, text_width);
            ImGui::SetCursorScreenPos({at.x + pad, end.y - pad - button});
            const auto unavailable = [&](const char* why) {
                ImGui::Dummy({text_width, (button - FontSize(fonts.body)) * 0.5f});
                ImGui::SetCursorScreenPos({at.x + pad, ImGui::GetCursorScreenPos().y});
                TextColored(fonts.body, color::dim, why);
            };
            if (n == 0)
            {
                if (!status.info.discord) unavailable(texts::T("match_setup.discord.not_offered"));
                else
                {
                    ImGui::BeginDisabled(status.busy);
                    if (DiscordButton("online_connect", texts::T("match_setup.discord.connect"), {text_width, button},
                                      true))
                        competitive_->SignIn();
                    ImGui::EndDisabled();
                }
            }
            else if (!status.info.mii) unavailable(texts::T("match_setup.choices.no_mii"));
            else if (PrimaryButton("online_create", texts::T("match_setup.profile_page.create.button"),
                                   {text_width, button}, !status.busy, Icon::Person))
                OpenProfileMaker(false);
        }
        ImGui::SetCursorScreenPos(top);
        ImGui::Dummy({width, height});
        ImGui::Dummy({0, Dp(2)});
        const char* import = texts::T("match_setup.profile_page.import.link");
        const float link = GhostWidth(import, Icon::Copy);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - link) * 0.5f));
        if (GhostButton("profile_import", import, {link, Dp(34)}, config_ok_, Icon::Copy)) profile_import_prompt_ = true;
    }

    // ------------------------------------------------------- profile maker
    // Create a profile (signed out), or finish one a Discord sign-in made (its
    // name and face chosen here): one name, which the game's Mii takes too,
    // and a face, a new one or that of a Mii of this computer.
    void OpenProfileMaker(bool finish)
    {
        maker_ = ProfileMaker{};
        maker_.open = true;
        maker_.finish = finish;
        maker_.created = std::int64_t(std::time(nullptr));
        const std::uint32_t seed = std::random_device{}();
        maker_.color = int(seed % 12);
        RollMakerFace(seed);
        if (finish) maker_.name = competitive_status_.player.name;
        page_ = PageOnlineSettings;
        page_scroll_top_ = true;
    }

    // A new random face for the maker's new Mii, from the Wii's own Mii
    // maker (its favourite colour stays the maker's).
    void RollMakerFace(std::uint32_t seed)
    {
        maker_.seed = seed;
        maker_.face = platform::mii::ReadMiiFace(platform::MakeRandomMii("Mii", seed, maker_.created));
    }

    // The Mii of this computer the maker took, if it is still there.
    const platform::InstalledMii* MakerChosenMii()
    {
        if (!maker_.chosen) return nullptr;
        for (const auto& mii : InstalledMiis().miis)
            if (mii.create_id == *maker_.chosen) return &mii;
        return nullptr;
    }

    // The name the Mii has in the game: the profile's, fitted to 10 characters.
    std::string MakerMiiName() const
    {
        const std::string name = TrimmedProfileName(maker_.name);
        return name.empty() ? std::string() : platform::FitMiiName(name);
    }

    // The maker's new Mii as its record (its create ID from `created`).
    // Throws for a name ValidMiiName refuses.
    std::string MakerNewRecord(std::int64_t created)
    {
        auto record = platform::MakeRandomMii(MakerMiiName(), maker_.seed, created, maker_.color);
        auto face = maker_.face;
        face.favorite_color = maker_.color;
        platform::mii::WriteMiiFace(face, record);
        return record;
    }

    // The Miis of this computer a new profile may start from: none that plays
    // for a profile, nor one with a friend code the server hasn't said about
    // (it asks only while signed out).
    std::vector<platform::InstalledMii> FreeMiis()
    {
        const auto& asked = competitive_status_.known_asked;
        std::vector<platform::InstalledMii> free;
        for (const auto& mii : InstalledMiis().miis)
        {
            const bool told = !mii.friend_code
                || std::find(asked.begin(), asked.end(), std::uint32_t(mii.friend_code)) != asked.end();
            if (told && !ProfileOfMii(mii)) free.push_back(mii);
        }
        return free;
    }

    // What keeps the maker from going on, told beside its button; empty when it can.
    std::string MakerBlocker()
    {
        const std::string name = TrimmedProfileName(maker_.name);
        if (!ValidProfileName(maker_.name))
            return name.empty() ? texts::T("match_setup.profile_page.create.choose_name")
                                : texts::T("match_setup.profile_page.create.name_rule");
        const auto& status = competitive_status_;
        const bool own = maker_.finish && name == status.player.name;
        if (!own && status.name_free && !*status.name_free && status.checked_name == name)
            return texts::T("match_setup.profile_page.create.name_taken");
        if (maker_.chosen) return MakerChosenMii() ? std::string() : texts::T("match_setup.profile_page.create.gone");
        if (!platform::ValidMiiName(MakerMiiName())) return texts::T("match_setup.profile_page.create.mii_name");
        const auto& miis = InstalledMiis();
        using platform::MiiDatabaseState;
        if (miis.state == MiiDatabaseState::Damaged) return texts::T("match_setup.wizard.blockers.damaged");
        if (miis.state == MiiDatabaseState::Unusable) return miis.error;
        if (miis.Full()) return texts::T("match_setup.wizard.blockers.full");
        return {};
    }

    void ProfileMakerPage()
    {
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        PageHeader(maker_.finish ? texts::T("match_setup.profile_page.finish.title")
                                 : texts::T("match_setup.profile_page.create.page_title"),
                   maker_.finish ? texts::T("match_setup.profile_page.finish.about")
                                 : texts::T("match_setup.profile_page.create.page_about"));
        BeginCard("##profile_maker", nullptr);
        const float width = ImGui::GetContentRegionAvail().x;
        if (BeginRows("##profile_maker_rows"))
        {
            Row(texts::T("match_setup.profile_page.create.name"), texts::T("match_setup.profile_page.create.name_about"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputTextWithHint("##profile_name", texts::T("match_setup.profile_page.create.name_hint"),
                                         &maker_.name))
                maker_.typed_at = SDL_GetTicks();
            // Whether it is free, asked once the name rests a moment.
            const std::string name = TrimmedProfileName(maker_.name);
            const bool own = maker_.finish && name == status.player.name;
            if (status.info.Profiles() && ValidProfileName(maker_.name) && !own && name != maker_.asked_name
                && SDL_GetTicks() - maker_.typed_at >= 400)
            {
                maker_.asked_name = name;
                competitive_->CheckName(name);
            }
            if (!own && status.checked_name == name && status.name_free)
            {
                ImGui::Dummy({0, Dp(2)});
                TextColored(fonts.caption, *status.name_free ? color::success : color::warning,
                            *status.name_free ? texts::T("match_setup.profile_page.create.name_free")
                                              : texts::T("match_setup.profile_page.create.name_taken"));
            }

            EndRows();
        }
        {
            // The face, the whole width: the Mii, its colour, another face or the editor.
            ImGui::Dummy({0, Dp(6)});
            const ImVec2 line = ImGui::GetCursorScreenPos();
            draw->AddRectFilled(line, {line.x + width, line.y + Dp(1)}, Col(color::border));
            ImGui::Dummy({width, Dp(12)});
            TextColored(fonts.label, color::text, texts::T("match_setup.profile_page.create.face"));
            TextColored(fonts.caption, color::muted, texts::T("match_setup.profile_page.create.face_about"));
            ImGui::Dummy({0, Dp(8)});
            const auto* chosen = MakerChosenMii();
            const float figure = Dp(112);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            std::string preview(platform::mii::kMiiRecordBytes, '\0');
            int colour = maker_.color;
            if (chosen)
            {
                preview = chosen->record;
                colour = chosen->favorite_color;
            }
            else
            {
                auto face = maker_.face;
                face.favorite_color = maker_.color;
                platform::mii::WriteMiiFace(face, preview);
            }
            DrawMii(draw, {at.x + figure * 0.5f, at.y + figure * 0.5f}, figure, colour, preview, 2);
            ImGui::SetCursorScreenPos({at.x + figure + Dp(24), at.y});
            ImGui::BeginGroup();
            if (chosen)
            {
                TextColored(fonts.label, color::text,
                            texts::T("match_setup.profile_page.create.using", {{"mii", chosen->name}}).c_str());
                ImGui::Dummy({0, Dp(6)});
                const char* fresh = texts::T("match_setup.profile_page.create.new_face");
                if (SecondaryButton("maker_new_face", fresh, {GhostWidth(fresh, Icon::Refresh) + Dp(16), Dp(36)}, true,
                                    Icon::Refresh))
                    maker_.chosen.reset();
            }
            else
            {
                TextColored(fonts.label, color::text, texts::T("match_setup.mii.favourite_colour"));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(4));
                TextColored(fonts.caption, color::muted, kMiiColorNames[maker_.color]);
                ImGui::Dummy({0, Dp(2)});
                const float swatches = ImGui::GetContentRegionAvail().x;
                MiiColorSwatches("##maker_colour", &maker_.color, Dp(30), swatches >= Dp(30 * 12 + 10 * 11) ? 12 : 6);
                ImGui::Dummy({0, Dp(6)});
                const char* another = texts::T("match_setup.wizard.another_face");
                if (SecondaryButton("maker_another_face", another, {GhostWidth(another, Icon::Refresh) + Dp(16), Dp(36)},
                                    true, Icon::Refresh))
                    RollMakerFace(std::random_device{}());
                ImGui::SameLine(0, Dp(10));
                const char* edit = texts::T("match_setup.wizard.edit_face");
                if (SecondaryButton("maker_edit_face", edit, {GhostWidth(edit, Icon::Paint) + Dp(16), Dp(36)}, true,
                                    Icon::Paint))
                    OpenMiiEditor(true, preview, false, true);
            }
            ImGui::EndGroup();
            const float bottom = std::max(ImGui::GetItemRectMax().y, at.y + figure);
            ImGui::SetCursorScreenPos({at.x, bottom});
            ImGui::Dummy({0, Dp(4)});
            // Or the face of a Mii this computer has (one that plays for no profile).
            const auto free = FreeMiis();
            if (!free.empty())
            {
                ImGui::Dummy({0, Dp(6)});
                TextColored(fonts.caption, color::muted, texts::T("match_setup.profile_page.create.or_mii"));
                ImGui::Dummy({0, Dp(2)});
                if (MiiGrid("##maker_miis", free, maker_.chosen) && !maker_.typed_at && maker_.name.empty())
                    if (const auto* mii = MakerChosenMii()) maker_.name = mii->name;
            }
        }
        ImGui::Dummy({0, Dp(10)});
        if (!maker_.finish)
            TextWrappedColored(fonts.caption, color::dim, texts::T("match_setup.profile_page.create.no_discord"));
        const std::string error = maker_.finish ? status.profile_error : status.mii_error;
        if (!error.empty() && maker_.tried)
        {
            ImGui::Dummy({0, Dp(8)});
            Callout(error + (error.back() == '.' ? "" : "."), color::danger, Icon::Error);
        }
        ImGui::Dummy({0, Dp(14)});
        const std::string blocker = MakerBlocker();
        const ImVec2 bar = ImGui::GetCursorScreenPos();
        const bool busy = status.busy || maker_.creating || maker_.saving;
        if (!maker_.finish && GhostButton("maker_back", texts::T("match_setup.buttons.back"), Dp(110, 42), !busy, Icon::Back))
            maker_ = ProfileMaker{};
        const float go = Dp(200);
        if (!blocker.empty() && (maker_.typed_at || maker_.chosen))
        {
            ImGui::SameLine(0, Dp(16));
            ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, bar.y + (Dp(42) - FontSize(fonts.caption)) * 0.5f});
            TextColored(fonts.caption, color::warning, blocker.c_str());
        }
        ImGui::SetCursorScreenPos({bar.x + width - go, bar.y});
        const bool ready = blocker.empty() && !busy
            && (maker_.finish ? status.state == CompetitiveService::State::SignedIn
                              : status.state == CompetitiveService::State::SignedOut);
        const char* label = busy ? texts::T("match_setup.profile_page.create.working")
            : maker_.finish     ? texts::T("match_setup.buttons.save")
                                : texts::T("match_setup.profile_page.create.button");
        if (PrimaryButton("maker_go", label, {go, Dp(42)}, ready, maker_.finish ? Icon::Check : Icon::Play, ready))
            FinishProfileMaker();
        EndCard();
    }

    // The maker's Mii in this computer's game save: the chosen one, or the new
    // one made now (once: a second try uses it). Its record; empty after a failure, told.
    std::string MakeMakerMii()
    {
        if (const auto* mii = MakerChosenMii()) return mii->record;
        try
        {
            const auto folder = SaveFolder();
            std::string record;
            // A create ID already there is made again: another one, the same face.
            for (int attempt = 0;; ++attempt)
            {
                record = MakerNewRecord(std::int64_t(std::time(nullptr)) + 4 * attempt);
                try
                {
                    platform::AddMii(record, folder, SaveTitleOf(Release()), folder / "backups");
                    ForgetInstalledMiis();
                    break;
                }
                catch (const platform::MiiCreateIdTaken&) { if (attempt == 2) throw; }
            }
            maker_.chosen = platform::MiiRecordCreateId(record);
            return record;
        }
        catch (const std::exception& e)
        {
            Notice(texts::T("match_setup.mii.not_made", {{"error", e.what()}}), NoticeKind::Error);
            return {};
        }
    }

    // Create: the Mii, then the profile by its name; the Mii becomes the
    // profile's when it succeeds (UpdateProfileMaker). Finish: the Mii
    // becomes the profile's now, then the name it chose.
    void FinishProfileMaker()
    {
        maker_.tried = true;
        const std::string record = MakeMakerMii();
        if (record.empty()) return;
        const std::string name = TrimmedProfileName(maker_.name);
        if (!maker_.finish)
        {
            maker_.creating = true;
            competitive_->MiiSignIn(name);
            return;
        }
        const auto& status = competitive_status_;
        ChooseOnlineMii(platform::MiiCreateIdText(platform::MiiRecordCreateId(record)));
        maker_.serial = status.profile_serial;
        bool asked = false;
        if (status.info.Profiles())
        {
            competitive_->ChooseMii(record);
            asked = true;
        }
        if (name != status.player.name)
        {
            renaming_to_ = name;
            rename_serial_ = status.profile_serial;
            competitive_->Rename(name);
            asked = true;
        }
        if (asked) maker_.saving = true;
        else maker_ = ProfileMaker{};
    }

    // Every frame: the maker's Create or Save that ended. Created, the Mii is
    // the profile's own (online.mii) and its face the profile's; saved, the
    // maker closes unless the name was refused (the page tells why).
    void UpdateProfileMaker()
    {
        const auto& status = competitive_status_;
        if (!maker_.open || status.busy) return;
        if (maker_.creating)
        {
            maker_.creating = false;
            const auto* mii = MakerChosenMii();
            if (status.state != CompetitiveService::State::SignedIn || !mii) return;
            ChooseOnlineMii(platform::MiiCreateIdText(mii->create_id));
            if (status.info.Profiles()) competitive_->ChooseMii(mii->record);
            // A Mii made on a try whose name was taken has that name: it takes the profile's.
            RenameOwnMii();
            if (!capture_)
                Notice(texts::T("match_setup.wizard.online_as", {{"name", status.player.name}}), NoticeKind::Success);
            maker_ = ProfileMaker{};
            return;
        }
        if (maker_.saving && status.profile_serial != maker_.serial)
        {
            maker_.saving = false;
            if (status.profile_error.empty()) maker_ = ProfileMaker{};
        }
    }

    // Every frame: a new sign-in (Status::sign_ins). Continue's Mii is the
    // profile's own; a profile Discord just made gets its name and face
    // chosen (the maker, as Finish); a Discord account that brought its
    // profile back says so.
    void UpdateSignIns()
    {
        const auto& status = competitive_status_;
        if (status.sign_ins == seen_sign_ins_) return;
        seen_sign_ins_ = status.sign_ins;
        if (status.state != CompetitiveService::State::SignedIn) return;
        if (continuing_)
        {
            const auto id = *continuing_;
            continuing_.reset();
            ChooseOnlineMii(platform::MiiCreateIdText(id));
            for (const auto& mii : InstalledMiis().miis)
                if (mii.create_id == id && status.info.Profiles()) competitive_->ChooseMii(mii.record);
            if (!capture_)
                Notice(texts::T("match_setup.profile_page.welcome_back", {{"name", status.player.name}}),
                       NoticeKind::Success);
        }
        else if (status.created)
            OpenProfileMaker(true);
        else if (status.welcome_back && !capture_)
            Notice(texts::T("match_setup.profile_page.welcome_back", {{"name", status.player.name}}),
                   NoticeKind::Success);
    }

    // Every frame: what the server keeps to bring the signed-in profile back
    // (Status::restore), used when this computer's game save lacks it. The
    // slot of this game version's Mii, with its Mii (the profile's face when
    // it is that Mii, else the slot's own copy), goes into the save; without
    // one, the profile's face alone is added. That Mii is the profile's own.
    void UpdateRestore()
    {
        const auto& status = competitive_status_;
        if (status.restore_serial == seen_restore_ || status.state != CompetitiveService::State::SignedIn) return;
        seen_restore_ = status.restore_serial;
        if (capture_) return;
        const auto& restore = status.restore;
        const auto& miis = InstalledMiis().miis;
        const auto here = [&](const std::array<unsigned char, 8>& id) {
            return std::find_if(miis.begin(), miis.end(),
                                [&](const platform::InstalledMii& mii) { return mii.create_id == id; });
        };
        const auto title = SaveTitleOf(Release());
        std::string game;
        for (const int shift : {24, 16, 8, 0}) game += char(Release().title_id >> shift);
        const auto folder = SaveFolder();
        try
        {
            for (const auto& saved : restore.slots)
            {
                if (saved.game != game) continue;
                const auto slot = platform::ReadOnlineSlot(saved.bytes);
                if (!slot) continue;
                const auto mii = here(slot->create_id);
                // The save has it already: the game's own copy is newer.
                if (mii != miis.end() && !mii->slot.empty())
                {
                    if (draft_.online_mii.empty()) ChooseOnlineMii(platform::MiiCreateIdText(slot->create_id));
                    return;
                }
                std::string record;
                if (restore.mii.size() == platform::mii::kMiiRecordBytes
                    && platform::MiiRecordCreateId(restore.mii) == slot->create_id)
                    record = restore.mii;
                else if (mii != miis.end()) record = mii->record;
                else
                {
                    record = slot->bytes.substr(platform::kOnlineSlotMiiOffset, platform::mii::kMiiRecordBytes);
                    if (platform::MiiRecordCreateId(record) != slot->create_id) continue;
                }
                platform::RestoreOnlineSlot(*slot, record, folder, title, folder / "backups");
                ForgetInstalledMiis();
                ChooseOnlineMii(platform::MiiCreateIdText(slot->create_id));
                Notice(texts::T("match_setup.profile_page.restored", {{"name", status.player.name}}),
                       NoticeKind::Success);
                return;
            }
            // No slot for this game version: the profile's face, to go online with.
            if (restore.mii.size() == platform::mii::kMiiRecordBytes && draft_.online_mii.empty())
            {
                const auto id = platform::MiiRecordCreateId(restore.mii);
                if (here(id) == miis.end())
                {
                    platform::AddMii(restore.mii, folder, title, folder / "backups");
                    ForgetInstalledMiis();
                }
                ChooseOnlineMii(platform::MiiCreateIdText(id));
            }
        }
        catch (const std::exception& e)
        {
            Notice(texts::T("match_setup.profile_page.not_restored", {{"error", e.what()}}), NoticeKind::Error);
        }
    }

    // The player's own Mii (online.mii), saved at once without the other
    // changes not saved yet; empty: none chosen.
    void ChooseOnlineMii(const std::string& id)
    {
        SaveNow([&](Settings& settings) { settings.online_mii = id; });
    }

    // This computer's Miis as tiles, the one `chosen` marked; true when a
    // click chose one (into `chosen`).
    bool MiiGrid(const char* id, const std::vector<platform::InstalledMii>& miis,
                 std::optional<std::array<unsigned char, 8>>& chosen_id)
    {
        bool clicked = false;
        ImGui::PushID(id);
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x, gap = Dp(10);
        const int columns = width > Dp(640) ? 3 : 2;
        const float tile = (width - gap * float(columns - 1)) / float(columns), height = Dp(62), figure = Dp(40);
        for (std::size_t n = 0; n < miis.size(); ++n)
        {
            const auto& mii = miis[n];
            if (n % std::size_t(columns)) ImGui::SameLine(0, gap);
            ImGui::PushID(int(n));
            const ImVec2 min = ImGui::GetCursorScreenPos(), max{min.x + tile, min.y + height};
            if (ImGui::InvisibleButton("##mii", {tile, height}))
            {
                chosen_id = mii.create_id;
                clicked = true;
            }
            const bool chosen = chosen_id && *chosen_id == mii.create_id;
            const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 14.0f);
            const float active = Animate(ImGui::GetID("##chosen"), chosen, 12.0f);
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            draw->AddRectFilled(min, max, Col(Mix(Mix(color::field, color::card_hover, hover), color::accent_dark, active)),
                                Dp(12));
            draw->AddRect(min, max, Col(Mix(color::border, color::accent, active), 0.6f + 0.4f * active), Dp(12), 0,
                          Dp(1.0f + 0.5f * active));
            DrawMii(draw, {min.x + Dp(12) + figure * 0.5f, min.y + height * 0.5f}, figure, mii.favorite_color,
                       mii.record);
            const float text = min.x + Dp(12) + figure + Dp(12);
            const float block = FontSize(fonts.label) + Dp(2) + FontSize(fonts.caption);
            const float top = min.y + (height - block) * 0.5f;
            DrawClipped(draw, fonts.label, {text, top}, max.x - text - Dp(30), Mix(color::text, color::accent_hi, active),
                        mii.name);
            DrawClipped(draw, fonts.caption, {text, top + FontSize(fonts.label) + Dp(2)}, max.x - text - Dp(30),
                        color::muted,
                        mii.friend_code ? texts::T("match_setup.mii.friend_code",
                                                   {{"code", platform::FriendCodeText(mii.friend_code)}})
                                        : std::string(texts::T("match_setup.mii.no_friend_code")));
            if (active > 0.0f)
                DrawIcon(draw, Icon::Check, {max.x - Dp(18), min.y + height * 0.5f}, Dp(16), Col(color::accent, active));
            ImGui::PopID();
        }
        ImGui::PopID();
        return clicked;
    }

    // ------------------------------------------------------------ Mii editor
    // A Mii's face, name and favourite colour edited on the page it was
    // opened from (MiiEditorPage, until Save or Cancel), drawn as the game
    // draws its Miis from the game image's Mii parts: no Mii Channel needed.
    // A new Mii of Play with a Mii's first step takes its face; one of this
    // computer's is saved in its own slot by its create ID (ReplaceMii), so
    // it stays who it is online.
    void OpenMiiEditor(bool for_wizard, const std::string& record, bool add, bool name_locked)
    {
        if (record.size() != platform::mii::kMiiRecordBytes) return;
        MiiEditor editor;
        editor.open = true;
        editor.page = page_;
        editor.target = for_wizard ? MiiEditor::Target::Maker : MiiEditor::Target::Database;
        if (!for_wizard)
        {
            editor.folder = SaveFolder();
            editor.title = SaveTitleOf(Release());
        }
        editor.record = record;
        editor.add = add;
        editor.name_locked = name_locked;
        editor.face = editor.original_face = platform::mii::ReadMiiFace(record);
        editor.name = editor.original_name = for_wizard ? MakerMiiName() : platform::MiiRecordName(record);
        mii_editor_ = std::move(editor);
        page_scroll_top_ = true;
    }

    void CloseMiiEditor()
    {
        mii_editor_ = MiiEditor{};
        page_scroll_top_ = true;
    }

    // What keeps Save from going: a name the Mii library refuses (a new Mii
    // of Play with a Mii may have none yet: its first step names it), or a
    // face it can't draw.
    std::string MiiEditorBlocker() const
    {
        const auto& editor = mii_editor_;
        const bool unnamed = editor.target == MiiEditor::Target::Maker && editor.name.empty();
        if (!unnamed && !platform::ValidMiiName(editor.name)) return texts::T("match_setup.editor.name_rule");
        if (!platform::mii::ValidMiiFace(editor.face)) return texts::T("match_setup.editor.face_blocked");
        return {};
    }

    bool MiiEditorChanged() const
    {
        return !SameFace(mii_editor_.face, mii_editor_.original_face) || mii_editor_.name != mii_editor_.original_name;
    }

    void SaveMiiEditor()
    {
        auto& editor = mii_editor_;
        if (!MiiEditorBlocker().empty()) return;
        if (editor.target == MiiEditor::Target::Maker)
        {
            maker_.face = editor.face;
            maker_.color = editor.face.favorite_color;
            CloseMiiEditor();
            return;
        }
        auto record = editor.record;
        platform::mii::WriteMiiFace(editor.face, record);
        try
        {
            if (!editor.name_locked) platform::SetMiiRecordName(record, editor.name);
            const auto& folder = editor.folder;
            const auto title = editor.title.value_or(SaveTitleOf(Release()));
            // The player's own Mii, not made here yet, is added with this face.
            bool added = false;
            if (editor.add)
                try
                {
                    platform::AddMii(record, folder, title, folder / "backups");
                    added = true;
                }
                catch (const platform::MiiCreateIdTaken&) {}
            if (!added) platform::ReplaceMii(record, folder, title, folder / "backups");
            ForgetInstalledMiis();
            // The profile's own Mii: its face is the profile's everywhere.
            if (const auto own = PlayerOwnMii(); own && own->id == platform::MiiRecordCreateId(record)
                && competitive_status_.info.Profiles())
                competitive_->ChooseMii(record);
            Notice(texts::T("match_setup.editor.saved", {{"mii", platform::MiiRecordName(record)}}),
                   NoticeKind::Success);
            CloseMiiEditor();
        }
        catch (const platform::MiiNotFound&)
        {
            Notice(texts::T("match_setup.editor.gone"), NoticeKind::Error);
        }
        catch (const std::exception& error)
        {
            Notice(texts::T("match_setup.editor.not_saved", {{"error", error.what()}}), NoticeKind::Error);
        }
    }

    // An editor's page header (the Mii editor, a match preset's): a back
    // button before the title, which leaves as Cancel does, and its text.
    // Whether back was pressed.
    bool EditorHeader(const char* id, const char* title, const char* text)
    {
        const auto& fonts = CurrentFonts();
        const float back = Dp(40);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, {back, back});
        const bool over = ImGui::IsItemHovered();
        if (over) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (over) ImGui::SetTooltip("%s", texts::T("match_setup.buttons.back_without_saving"));
        auto* header = ImGui::GetWindowDrawList();
        const ImVec2 center{at.x + back * 0.5f, at.y + back * 0.5f};
        header->AddCircleFilled(center, back * 0.5f, Col(color::text, over ? 0.12f : 0.05f));
        header->AddCircle(center, back * 0.5f - Dp(0.5f), Col(color::border), 0, Dp(1));
        DrawIcon(header, Icon::Back, center, Dp(18), Col(over ? color::text : color::muted));
        ImGui::SameLine(0, Dp(14));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (back - FontSize(fonts.title)) * 0.5f - Dp(2));
        TextColored(fonts.title, color::text, title);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(2));
        TextWrappedColored(fonts.body, color::muted, text);
        ImGui::Dummy({0, Dp(8)});
        return pressed;
    }

    // The editor: the Mii's head large with its expression, name and
    // favourite colour on the left; its parts by section on the right, the
    // types as thumbnails of the head with each; Cancel, Undo, Random, Start
    // over and Save under them.
    void MiiEditorPage()
    {
        auto& editor = mii_editor_;
        const auto& fonts = CurrentFonts();
        const bool wizard = editor.target == MiiEditor::Target::Maker;
        ++mii_editor_frames_;
        if (EditorHeader("##mii_editor_back",
                         wizard ? texts::T("match_setup.editor.title_new") : texts::T("match_setup.editor.title_edit"),
                         wizard ? texts::T("match_setup.editor.about_new") : texts::T("match_setup.editor.about_edit")))
        {
            CloseMiiEditor();
            return;
        }
        const MiiEditor::Step before{editor.face, editor.name};
        BeginCard("##mii_editor", nullptr);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const float left = std::clamp(width * 0.32f, Dp(220), Dp(256)), gap = Dp(26);

        // Left: the head, with the expression it shows.
        const float top = ImGui::GetCursorScreenPos().y;
        ImGui::BeginGroup();
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float side = left;
            const platform::mii::Expression expression{editor.expression};
            const MiiHeadLook look{HeadPixels(side), MiiHeadBackground::Favorite, expression};
            MiiFaces::Texture head = mii_faces_ ? mii_faces_->Head(editor.face, look, 2) : nullptr;
            if (head)
            {
                editor.shown = editor.face;
                editor.shown_expression = editor.expression;
                editor.has_shown = true;
            }
            else if (editor.has_shown && mii_faces_)
                head = mii_faces_->Cached(editor.shown, {look.size, MiiHeadBackground::Favorite,
                                                         platform::mii::Expression{editor.shown_expression}});
            const ImVec2 max{at.x + side, at.y + side};
            if (head)
                draw->AddImageRounded(TextureId(head), at, max, {0, 0}, {1, 1}, IM_COL32_WHITE, Dp(18));
            else
            {
                const ImVec4 fill = MiiColor(editor.face.favorite_color);
                draw->AddRectFilled(at, max, Col(fill), Dp(18));
                const bool light = 0.2126f * fill.x + 0.7152f * fill.y + 0.0722f * fill.z > 0.6f;
                const auto parts = mii_faces_ ? mii_faces_->PartsState() : MiiFaces::Parts::None;
                const bool loading = parts == MiiFaces::Parts::Loading || parts == MiiFaces::Parts::Ready;
                if (loading)
                    Spinner({at.x + side * 0.5f, at.y + side * 0.5f}, Dp(18), Col(light ? color::window : color::text));
                else
                    DrawIcon(draw, Icon::Person, {at.x + side * 0.5f, at.y + side * 0.5f}, side * 0.5f,
                             Col(light ? color::window : color::text));
            }
            draw->AddRect(at, max, Col(color::border), Dp(18), 0, Dp(1));
            ImGui::Dummy({side, side});
            if (mii_faces_ && mii_faces_->PartsState() == MiiFaces::Parts::Failed)
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
                TextWrappedColored(fonts.caption, color::warning, texts::T("match_setup.editor.parts_unreadable"));
                ImGui::PopTextWrapPos();
            }
            ImGui::Dummy({0, Dp(4)});
            Pills("##mii_expression",
                  {texts::T("match_setup.editor.expressions.normal"), texts::T("match_setup.editor.expressions.smile"),
                   texts::T("match_setup.editor.expressions.angry"), texts::T("match_setup.editor.expressions.sad"),
                   texts::T("match_setup.editor.expressions.surprised"),
                   texts::T("match_setup.editor.expressions.blink"),
                   texts::T("match_setup.editor.expressions.open_mouth")},
                  &editor.expression, side, fonts.caption, Dp(26));
        }
        ImGui::EndGroup();
        const float left_height = ImGui::GetItemRectSize().y;

        // Right: the sections, and the chosen one's parts.
        ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        const float right = ImGui::GetContentRegionAvail().x;
        const auto& sections = MiiEditorSections();
        std::vector<const char*> names;
        for (const auto& section : sections) names.push_back(section.name);
        Pills("##mii_sections", names, &editor.section, right, fonts.body, Dp(34));
        ImGui::Dummy({0, Dp(4)});
        // As tall as the left column: the head stays in view while the parts scroll.
        const float panel = std::max(Dp(300), top + left_height - ImGui::GetCursorScreenPos().y);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(2, 4));
        ImGui::BeginChild("##mii_parts", {right, panel}, ImGuiChildFlags_AlwaysUseWindowPadding);
        if (editor.section_shown != editor.section)
        {
            ImGui::SetScrollY(0.0f);
            editor.section_shown = editor.section;
        }
        for (const auto& row : sections[std::size_t(std::clamp(editor.section, 0, int(sections.size()) - 1))].rows)
            MiiEditorRowControl(row, editor.face);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        // Under both: the steps (Undo, Start over, Random) at the left, Cancel
        // and Save Mii at the right, all of one height.
        ImGui::Dummy({0, Dp(14)});
        const float button = Dp(36);
        const ImVec2 bar = ImGui::GetCursorScreenPos();
        // Undo and Start over are steps of their own, not changes to note.
        bool stepped = false;
        // The steps share the left half, framed like Cancel and Save Mii.
        const float step = (width * 0.5f - Dp(8) * 2) / 3;
        if (SecondaryButton("mii_editor_undo", texts::T("match_setup.editor.undo"), {step, button},
                            !editor.undo.empty(), Icon::Back))
        {
            editor.face = editor.undo.back().face;
            editor.name = editor.undo.back().name;
            editor.undo.pop_back();
            stepped = true;
        }
        ImGui::SameLine(0, Dp(8));
        if (SecondaryButton("mii_editor_reset", texts::T("match_setup.editor.start_over"), {step, button},
                            MiiEditorChanged(), Icon::Refresh))
        {
            editor.undo.push_back({editor.face, editor.name});
            editor.face = editor.original_face;
            editor.name = editor.original_name;
            stepped = true;
        }
        ImGui::SameLine(0, Dp(8));
        // A whole new face from the Wii's own Mii maker; the name and the
        // favourite colour stay, and Undo takes it back.
        if (SecondaryButton("mii_editor_random", texts::T("match_setup.editor.random"), {step, button}, true,
                            Icon::Dice))
        {
            editor.undo.push_back({editor.face, editor.name});
            const int color = editor.face.favorite_color;
            try
            {
                const auto random = platform::MakeRandomMii("Random", std::random_device{}(),
                                                            std::int64_t(std::time(nullptr)), color);
                editor.face = platform::mii::ReadMiiFace(random);
                editor.face.favorite_color = color;
            }
            catch (const std::exception& error)
            {
                editor.undo.pop_back();
                Notice(texts::T("match_setup.editor.no_random_face", {{"error", error.what()}}), NoticeKind::Error);
            }
            stepped = true;
        }
        const std::string blocker = MiiEditorBlocker();
        const char* save = wizard ? texts::T("match_setup.editor.use_face") : texts::T("match_setup.editor.save_mii");
        const float end_button = Dp(wizard ? 150 : 124);
        const float right_start = bar.x + width - end_button * 2 - Dp(8);
        if (!blocker.empty())
        {
            const ImVec2 size = TextSize(fonts.caption, blocker.c_str());
            DrawLabel(draw, fonts.caption, {right_start - Dp(14) - size.x, bar.y + (button - size.y) * 0.5f},
                      Col(color::warning), blocker.c_str());
        }
        ImGui::SetCursorScreenPos({right_start, bar.y});
        if (SecondaryButton("mii_editor_cancel", texts::T("match_setup.buttons.cancel"), {end_button, button}, true,
                            Icon::Close))
            CloseMiiEditor();
        ImGui::SameLine(0, Dp(8));
        if (PrimaryButton("mii_editor_save", save, {end_button, button},
                          blocker.empty() && (wizard || MiiEditorChanged()), Icon::Check))
            SaveMiiEditor();
        EndCard();
        if (!editor.open) return;
        if (stepped)
        {
            editor.gesture.clear();
            return;
        }

        // A change after a pause, or to another value, is a step Undo takes back.
        std::string changed;
        if (editor.name != before.name) changed = "name";
        for (const auto& field : platform::mii::kMiiFaceFields)
            if (editor.face.*field.value != before.face.*field.value) changed = field.name;
        if (!changed.empty())
        {
            if (changed != editor.gesture || editor.released) editor.undo.push_back(before);
            if (editor.undo.size() > 200) editor.undo.erase(editor.undo.begin());
            editor.gesture = changed;
            editor.released = false;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) editor.released = true;
    }

    // One row of a section: its label, and its thumbnails, swatches, slider or switch.
    void MiiEditorRowControl(const MiiEditorRow& row, platform::mii::MiiFace& face)
    {
        const auto& fonts = CurrentFonts();
        const float width = ImGui::GetContentRegionAvail().x - Dp(8);
        if (row.control == MiiControl::Name)
        {
            auto& editor = mii_editor_;
            TextColored(fonts.label, color::text, row.label);
            ImGui::Dummy({0, Dp(2)});
            ImGui::SetNextItemWidth(std::min(width, Dp(320)));
            ImGui::BeginDisabled(editor.name_locked);
            ImGui::InputTextWithHint("##mii_editor_name", texts::T("match_setup.mii.name_hint"), &editor.name);
            ImGui::EndDisabled();
            if (editor.name_locked)
                TextWrappedColored(fonts.caption, color::dim, texts::T("match_setup.editor.discord_name"));
            else if (!platform::ValidMiiName(editor.name))
                TextWrappedColored(fonts.caption, color::warning, texts::T("match_setup.editor.name_rule"));
            else
                TextWrappedColored(fonts.caption, color::dim, texts::T("match_setup.editor.name_about"));
            ImGui::Dummy({0, Dp(14)});
            return;
        }
        const auto& field = FaceField(row.field);
        int& value = face.*field.value;
        ImGui::PushID(row.field);
        // Without its part (no glasses, no mole...) the row has nothing to change.
        const bool off = row.needs && face.*FaceField(row.needs).value == FaceField(row.needs).min;
        ImGui::BeginDisabled(off);
        switch (row.control)
        {
        case MiiControl::Parts:
            TextColored(fonts.label, color::text, row.label);
            ImGui::Dummy({0, Dp(2)});
            MiiPartGrid(row, face, width);
            ImGui::Dummy({0, Dp(12)});
            break;
        case MiiControl::Colors:
        {
            TextColored(fonts.label, color::text, row.label);
            ImGui::Dummy({0, Dp(2)});
            const std::string name = row.field;
            const auto colour = [name](int v) {
                const auto rgb = platform::mii::MiiFieldColor(name, v).value_or(std::array<std::uint8_t, 3>{});
                return ImVec4{rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1.0f};
            };
            const int count = field.max - field.min + 1;
            const bool favourite = std::string_view(row.field) == "favorite_color";
            if (favourite)
            {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
                TextColored(fonts.caption, color::muted, kMiiColorNames[std::clamp(value, 0, 11)]);
                ImGui::Dummy({0, Dp(2)});
            }
            const float swatch = Dp(34);
            const int per_row = std::clamp(int((width + Dp(10)) / (swatch + Dp(10))), 1, count);
            ColorSwatches("##colours", &value, count, colour, favourite ? kMiiColorNames : nullptr, swatch,
                          per_row > 6 && per_row < count ? 6 : per_row, field.min);
            ImGui::Dummy({0, Dp(12)});
            break;
        }
        case MiiControl::Slider:
        case MiiControl::Height:
        {
            const bool height = row.control == MiiControl::Height;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float label = Dp(150);
            const float line = ImGui::GetFrameHeight();
            DrawLabel(ImGui::GetWindowDrawList(), fonts.body, {at.x, at.y + (line - FontSize(fonts.body)) * 0.5f},
                      Col(color::text), row.label);
            ImGui::SetCursorScreenPos({at.x + label, at.y});
            const int shown = height ? field.max - value + 1 : value - field.min + 1;
            const std::string text = std::to_string(shown);
            StepSlider("##slider", &value, field.min, field.max, width - label, text.c_str(), height);
            ImGui::Dummy({0, Dp(6)});
            break;
        }
        case MiiControl::Toggle:
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float line = ImGui::GetFrameHeight();
            DrawLabel(ImGui::GetWindowDrawList(), fonts.body, {at.x, at.y + (line - FontSize(fonts.body)) * 0.5f},
                      Col(color::text), row.label);
            ImGui::SetCursorScreenPos({at.x + Dp(150), at.y + (line - Dp(26)) * 0.5f});
            bool on = value != field.min;
            if (Toggle("##toggle", &on)) value = on ? field.max : field.min;
            ImGui::SetCursorScreenPos({at.x, at.y + line});
            ImGui::Dummy({0, Dp(10)});
            break;
        }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // A part's values as tiles, each the head with that value (the square
    // of the head the part is in), the chosen one ringed. The heads come from
    // the worker as the tiles show; a tile shows its number until then.
    void MiiPartGrid(const MiiEditorRow& row, platform::mii::MiiFace& face, float width)
    {
        const auto& field = FaceField(row.field);
        auto* draw = ImGui::GetWindowDrawList();
        const auto& fonts = CurrentFonts();
        const float gap = Dp(8);
        const int columns = std::max(3, int((width + gap) / (Dp(72) + gap)));
        const float tile = (width - gap * float(columns - 1)) / float(columns), pad = Dp(3);
        const MiiHeadLook look{HeadPixels((tile - 2 * pad) / (2.0f * row.crop.half)), MiiHeadBackground::Transparent};
        const ImVec2 uv0{row.crop.x - row.crop.half, row.crop.y - row.crop.half};
        const ImVec2 uv1{row.crop.x + row.crop.half, row.crop.y + row.crop.half};
        // Glasses, mustache and beard start with none.
        const bool none_first = std::string_view(row.field) == "glasses_type"
            || std::string_view(row.field) == "mustache_type" || std::string_view(row.field) == "beard_type";
        int& value = face.*field.value;
        for (int v = field.min; v <= field.max; ++v)
        {
            const int n = v - field.min;
            if (n % columns) ImGui::SameLine(0, gap);
            ImGui::PushID(v);
            const ImVec2 min = ImGui::GetCursorScreenPos(), max{min.x + tile, min.y + tile};
            if (ImGui::InvisibleButton("##part", {tile, tile})) value = v;
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const float hover = Animate(ImGui::GetItemID(), hovered, 14.0f);
            const float chosen = Animate(ImGui::GetID("##chosen"), value == v, 14.0f);
            if (ImGui::IsRectVisible(min, max))
            {
                draw->AddRectFilled(min, max, Col(Mix(Mix(color::field, color::card_hover, hover), color::accent_dark,
                                                      0.6f * chosen)),
                                    Dp(10));
                auto variant = face;
                variant.*field.value = v;
                const MiiFaces::Texture head = mii_faces_ ? mii_faces_->Head(variant, look, 0) : nullptr;
                if (head)
                    draw->AddImageRounded(TextureId(head), {min.x + pad, min.y + pad}, {max.x - pad, max.y - pad}, uv0,
                                          uv1, IM_COL32_WHITE, Dp(8));
                else
                {
                    const std::string number = std::to_string(n + 1);
                    const ImVec2 size = TextSize(fonts.body, number.c_str());
                    DrawLabel(draw, fonts.body, {min.x + (tile - size.x) * 0.5f, min.y + (tile - size.y) * 0.5f},
                              Col(color::dim), number.c_str());
                }
                if (none_first && v == field.min)
                {
                    const char* none = texts::T("match_setup.editor.part_none");
                    const ImVec2 size = TextSize(fonts.caption, none);
                    const ImVec2 at{min.x + (tile - size.x) * 0.5f, max.y - size.y - Dp(7)};
                    draw->AddRectFilled({at.x - Dp(7), at.y - Dp(2)}, {at.x + size.x + Dp(7), at.y + size.y + Dp(2)},
                                        Col(color::window, 0.85f), size.y);
                    DrawLabel(draw, fonts.caption, at, Col(color::text), none);
                }
                draw->AddRect(min, max, Col(Mix(color::border, color::accent, chosen), 0.6f + 0.4f * chosen), Dp(10), 0,
                              Dp(1.0f + chosen));
            }
            ImGui::PopID();
        }
    }

    // Online -> Player profile: one card with the player's profile (the
    // initial, or a Mii player's Mii; the name with what they are doing, how
    // they signed in, and Sign out) over rows for their Mii, the friend code,
    // Discord (its friends, or connecting it), the chime and the server.
    // Signed out: the ways to a profile, or Create's page.
    void OnlineSettingsPage()
    {
        using State = CompetitiveService::State;
        const auto& status = competitive_status_;
        if (status.state != State::SignedIn)
        {
            if (maker_.open && !maker_.finish && status.state != State::SigningIn) ProfileMakerPage();
            else OnlineWelcome();
            return;
        }
        // A profile Discord just made: its name and face first.
        if (maker_.open && maker_.finish)
        {
            ProfileMakerPage();
            return;
        }
        const auto& player = status.player;
        const bool mii_account = player.MiiAccount();
        PageHeader(texts::T("match_setup.profile_page.title"), texts::T("match_setup.profile_page.about"));
        const auto own = PlayerOwnMii();
        const auto& fonts = CurrentFonts();
        BeginCard("##online_profile", nullptr);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = Dp(64), avatar = Dp(58);
        const ImVec2 center{at.x + avatar * 0.5f, at.y + height * 0.5f};
        // The profile's face; an older profile without one shows its initial.
        if (own && own->record.size() == platform::mii::kMiiRecordBytes)
            DrawMii(draw, center, avatar, own->color, own->record);
        else
        {
            draw->AddCircleFilled(center, avatar * 0.5f, Col(color::accent, 0.16f));
            draw->AddCircle(center, avatar * 0.5f, Col(color::accent, 0.45f), 0, Dp(1.5f));
            std::string initial = player.name.empty() ? std::string("?") : player.name.substr(0, 1);
            if (const auto byte = static_cast<unsigned char>(initial[0]); byte >= 0x80)
                initial = player.name.substr(0, std::min<std::size_t>(byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2,
                                                                       player.name.size()));
            else
                initial[0] = char(std::toupper(byte));
            const ImVec2 letter = TextSize(fonts.title, initial.c_str());
            DrawLabel(draw, fonts.title, {center.x - letter.x * 0.5f, center.y - letter.y * 0.5f}, Col(color::accent),
                      initial.c_str());
        }
        // Switch profile (signing out), which also ends the Discord friends'
        // link on this computer; a Mii player is asked first (their Mii's
        // Continue brings them back).
        const char* switch_label = texts::T("match_setup.profile_page.switch");
        const ImVec2 sign_out{GhostWidth(switch_label, Icon::Close), Dp(38)};
        ImGui::SetCursorScreenPos({at.x + width - sign_out.x, center.y - sign_out.y * 0.5f});
        if (GhostButton("sign_out", switch_label, sign_out, !status.busy, Icon::Close))
        {
            if (mii_account) sign_out_prompt_ = true;
            else SignOutPlayer();
        }
        const float text_left = at.x + avatar + Dp(18);
        // Under the name: its Discord account, else its friend code.
        const std::string under = !player.discord.empty()
            ? texts::T("match_setup.profile.account_discord", {{"name", player.discord}})
            : player.test               ? std::string(texts::T("match_setup.test_player.title"))
            : !player.friend_code.empty() ? texts::T("match_setup.profile.account_mii", {{"code", player.friend_code}})
                                        : std::string(texts::T("match_setup.profile.account_mii_no_code"));
        const float block = FontSize(fonts.heading) + Dp(4) + FontSize(fonts.body);
        const float top = center.y - block * 0.5f;
        draw->PushClipRect({text_left, at.y}, {at.x + width - sign_out.x - Dp(16), at.y + height}, true);
        DrawLabel(draw, fonts.heading, {text_left, top}, Col(color::text), player.name.c_str());
        {
            // What the player is doing, as a pill after the name.
            const auto [word, tint] = PlayerActivity(status.match);
            const ImVec2 size = TextSize(fonts.caption, word);
            const ImVec2 pill{text_left + TextSize(fonts.heading, player.name.c_str()).x + Dp(12),
                              top + (FontSize(fonts.heading) - Dp(22)) * 0.5f};
            draw->AddRectFilled(pill, {pill.x + Dp(26) + size.x, pill.y + Dp(22)}, Col(tint, 0.14f), Dp(11));
            draw->AddCircleFilled({pill.x + Dp(11), pill.y + Dp(11)}, Dp(3.5f), Col(tint));
            DrawLabel(draw, fonts.caption, {pill.x + Dp(19), pill.y + (Dp(22) - size.y) * 0.5f}, Col(tint), word);
        }
        DrawLabel(draw, fonts.body, {text_left, top + FontSize(fonts.heading) + Dp(4)}, Col(color::muted),
                  under.c_str());
        draw->PopClipRect();
        ImGui::SetCursorScreenPos(at);
        ImGui::Dummy({width, height});
        ImGui::Dummy({0, Dp(8)});
        {
            const ImVec2 line = ImGui::GetCursorScreenPos();
            draw->AddRectFilled({line.x, line.y}, {line.x + width, line.y + Dp(1)}, Col(color::border));
            ImGui::Dummy({width, Dp(6)});
        }

        // A row's value: a dot in its colour, then the text.
        const auto value = [&](const std::string& text, const ImVec4& tint, const ImVec4* dot) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float line = FontSize(fonts.label);
            float x = start.x;
            if (dot)
            {
                draw->AddCircleFilled({x + Dp(4), start.y + line * 0.5f}, Dp(4), Col(*dot));
                x += Dp(16);
            }
            DrawLabel(draw, fonts.label, {x, start.y}, Col(tint), text.c_str());
            ImGui::Dummy({x - start.x + TextSize(fonts.label, text.c_str()).x, line});
        };
        // The rows' actions (Change and Edit of your Mii) start in one column
        // right after the widest of their values, so each reads with its value.
        {
            const float mii = own && (own->here || !ChosenOwnMii())
                ? Dp(30) + Dp(10) + TextSize(fonts.label, own->name.c_str()).x
                : TextSize(fonts.label, texts::T("match_setup.profile.no_mii")).x;
            const float name = TextSize(fonts.label, player.name.c_str()).x;
            profile_actions_ = std::max({mii, name, Dp(48)}) + Dp(18);
        }
        if (BeginRows("##online_settings_rows"))
        {
            NameRow();
            OwnMiiRow(own);

            // The friend code of the Mii that plays online, others in the tooltip.
            std::string code = player.friend_code;
            if (code.empty() && !player.wfc.empty()) code = player.wfc.front().friend_code;
            if (code.empty() && mii_account && own && own->friend_code)
                code = platform::FriendCodeText(own->friend_code);
            Row(texts::T("match_setup.profile.friend_code"),
                !code.empty() ? texts::T("match_setup.profile.friend_code_about")
                : mii_account ? texts::T("match_setup.profile.friend_code_mii_later")
                              : texts::T("match_setup.profile.friend_code_later"));
            if (code.empty())
                value(mii_account ? texts::T("match_setup.profile.after_first_game")
                                  : texts::T("match_setup.profile.none_yet"),
                      color::muted, nullptr);
            else
            {
                // On a button that copies it.
                const std::string shown =
                    code + (player.wfc.size() > 1 ? "  +" + std::to_string(player.wfc.size() - 1) : std::string());
                const float copy_width = TextSize(fonts.body, shown.c_str()).x + Dp(18) + Dp(8) + Dp(32);
                if (SecondaryButton("copy_friend_code", shown.c_str(), {copy_width, Dp(34)}, true, Icon::Copy)
                    && SDL_SetClipboardText(code.c_str()))
                    Notice(texts::T("match_setup.profile.code_copied", {{"code", code}}), NoticeKind::Success);
                if (ImGui::IsItemHovered() && !player.wfc.empty())
                {
                    std::string codes;
                    for (const auto& profile : player.wfc)
                        codes += (codes.empty() ? "" : "\n") + profile.friend_code + "  "
                            + (profile.mii.empty() ? profile.game : profile.mii + ", " + profile.game);
                    ImGui::SetTooltip("%s", codes.c_str());
                }
            }

            DiscordRow();

            // The flag beside the name (servers from 1.2.11 on say the choice).
            if (!player.country_choice.empty())
            {
                Row(texts::T("match_setup.profile.flag"), texts::T("match_setup.profile.flag_about"));
                FlagCombo(player, status.busy);
            }

            // The server: only the settings file changes it.
            std::string host = status.server;
            if (const auto scheme = host.find("://"); scheme != std::string::npos) host.erase(0, scheme + 3);
            const std::string where
                = texts::T("match_setup.profile.server_about",
                           {{"key", "online.profile_server"}, {"file", PathUtf8(file_.path.filename())}});
            Row(texts::T("match_setup.profile.server"), where.c_str());
            value(host + (status.info.version.empty() ? std::string() : "  ·  " + status.info.version), color::text,
                  &color::success);
            EndRows();
        }
        EndCard();
        // A change of the profile the server refused (a taken name...).
        if (!status.profile_error.empty() && !profile_renaming_)
            Callout(status.profile_error + (status.profile_error.back() == '.' ? "" : "."), color::warning,
                    Icon::Warning);
        // Connecting Discord was refused (a server before 1.3.0: that
        // Discord account has a profile of its own).
        if (!status.connect_error.empty())
        {
            std::string text = status.connect_error;
            if (!text.empty() && text.back() != '.') text += '.';
            if (status.connect_error.find("This Discord account") != std::string::npos)
                text = texts::T("match_setup.profile.discord_taken", {{"error", text}});
            Callout(text, color::warning, Icon::Warning);
        }
    }

    // The profile's name, and Rename (servers from 1.3.0): one name, unique
    // on the server, which the game's Mii takes too (UpdateRenames).
    void NameRow()
    {
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        Row(texts::T("match_setup.profile_page.rows.name"), texts::T("match_setup.profile_page.rows.name_about"));
        if (profile_renaming_)
        {
            const char* save = texts::T("match_setup.buttons.save");
            const char* cancel = texts::T("match_setup.buttons.cancel");
            const float buttons = ActionWidth(save, Icon::Check) + Dp(8) + GhostWidth(cancel, Icon::None) + Dp(8);
            ImGui::SetNextItemWidth(std::max(Dp(120), ImGui::GetContentRegionAvail().x - buttons));
            ImGui::InputTextWithHint("##rename", texts::T("match_setup.profile_page.create.name_hint"), &rename_text_);
            const std::string name = TrimmedProfileName(rename_text_);
            const bool changed = name != status.player.name;
            ImGui::SameLine(0, Dp(8));
            if (SecondaryButton("rename_save", save, {ActionWidth(save, Icon::Check), ImGui::GetFrameHeight()},
                                ValidProfileName(rename_text_) && changed && !status.busy, Icon::Check))
            {
                renaming_to_ = name;
                rename_serial_ = status.profile_serial;
                competitive_->Rename(name);
            }
            ImGui::SameLine(0, Dp(8));
            if (GhostButton("rename_cancel", cancel, {GhostWidth(cancel, Icon::None), ImGui::GetFrameHeight()}))
                profile_renaming_ = false;
            if (!ValidProfileName(rename_text_))
                TextColored(fonts.caption, color::warning, texts::T("match_setup.profile_page.create.name_rule"));
            else if (!status.profile_error.empty() && renaming_to_.empty())
                TextColored(fonts.caption, color::warning, status.profile_error.c_str());
            return;
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float line = std::max(Dp(30), FontSize(fonts.label));
        DrawLabel(ImGui::GetWindowDrawList(), fonts.label, {at.x, at.y + (line - FontSize(fonts.label)) * 0.5f},
                  Col(color::text), status.player.name.c_str());
        ImGui::Dummy({TextSize(fonts.label, status.player.name.c_str()).x, line});
        if (!status.info.Profiles()) return;
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos({at.x + profile_actions_, at.y + (line - Dp(32)) * 0.5f});
        const char* rename = texts::T("match_setup.profile_page.rows.rename");
        if (SecondaryButton("profile_rename", rename, {ActionWidth(rename, Icon::Paint), Dp(32)}, !status.busy,
                            Icon::Paint))
        {
            profile_renaming_ = true;
            rename_text_ = status.player.name;
        }
    }

    // The row of the profile's face: its Mii (the one chosen with the profile,
    // or for an older profile the one made from its account), and Edit.
    void OwnMiiRow(const std::optional<OwnMii>& own)
    {
        const auto& fonts = CurrentFonts();
        const bool chosen_kind = ChosenOwnMii();
        Row(texts::T("match_setup.profile_page.rows.face"),
            own && (own->here || !chosen_kind) ? texts::T("match_setup.profile_page.rows.face_about")
                                               : texts::T("match_setup.profile.own_mii.elsewhere"));
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float figure = Dp(30), line = std::max(figure, FontSize(fonts.label));
        if (own && (own->here || !chosen_kind))
        {
            DrawMii(draw, {at.x + figure * 0.5f, at.y + line * 0.5f}, figure, own->color, own->record);
            DrawLabel(draw, fonts.label, {at.x + figure + Dp(10), at.y + (line - FontSize(fonts.label)) * 0.5f},
                      Col(color::text), own->name.c_str());
            ImGui::Dummy({figure + Dp(10) + TextSize(fonts.label, own->name.c_str()).x, line});
        }
        else
        {
            const char* none = texts::T("match_setup.profile.no_mii");
            DrawLabel(draw, fonts.label, {at.x, at.y + (line - FontSize(fonts.label)) * 0.5f}, Col(color::muted), none);
            ImGui::Dummy({TextSize(fonts.label, none).x, line});
        }
        // Edit in the actions' column: its face in the Mii editor (an older
        // profile's Mii made from its account is added with it when it isn't
        // here yet). The name is the profile's: Rename changes it.
        if (own && (own->here || !chosen_kind) && own->record.size() == platform::mii::kMiiRecordBytes)
        {
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos({at.x + profile_actions_, at.y + (line - Dp(32)) * 0.5f});
            const char* edit = texts::T("match_setup.wizard.edit_face");
            if (SecondaryButton("own_mii_edit", edit, {ActionWidth(edit, Icon::Paint), Dp(32)}, true, Icon::Paint))
                OpenMiiEditor(false, own->record, !own->here, true);
        }
    }

    // The profile's Discord row: connected (as whom, its Discord friends, and
    // Disconnect from servers 1.3.0 on), or Connect Discord (the sign-in with
    // the session joins Discord to this profile), and its code while it runs.
    void DiscordRow()
    {
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        if (status.connecting_discord)
        {
            Row(texts::T("match_setup.profile.discord"), texts::T("match_setup.profile.discord_code"));
            TextColored(fonts.heading, color::accent, status.code.c_str());
            ImGui::SameLine(0, Dp(14));
            const float y = ImGui::GetCursorPosY() - Dp(4);
            ImGui::SetCursorPosY(y);
            const char* again = texts::T("match_setup.profile.open_again");
            if (GhostButton("connect_again", again, {GhostWidth(again, Icon::Link), Dp(32)}, !status.url.empty(),
                            Icon::Link))
                SDL_OpenURL(status.url.c_str());
            ImGui::SameLine(0, Dp(6));
            ImGui::SetCursorPosY(y);
            const char* cancel = texts::T("match_setup.buttons.cancel");
            if (GhostButton("connect_stop", cancel, {GhostWidth(cancel, Icon::None), Dp(32)}))
                competitive_->StopSignIn();
            return;
        }
        const auto& player = status.player;
        if (player.discord.empty())
        {
            Row(texts::T("match_setup.profile.discord"), status.info.discord
                                                             ? texts::T("match_setup.profile_page.rows.discord_about")
                                                             : texts::T("match_setup.discord.not_offered"));
            if (status.info.discord && !player.test
                && DiscordButton("connect_discord", texts::T("match_setup.discord.connect"), Dp(200, 36))
                && !status.busy)
                competitive_->SignIn();
            return;
        }
        // Connected: as whom, then its Discord friends.
        using DiscordState = DiscordFriends::State;
        const DiscordState discord = discord_friends_ ? discord_status_.state : DiscordState::Missing;
        const bool linked = discord == DiscordState::Ready;
        const bool waiting = discord == DiscordState::Connecting || discord == DiscordState::Linking;
        Row(texts::T("match_setup.profile.discord"), texts::T("match_setup.profile_page.rows.discord_connected_about"));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float line = std::max(Dp(30), FontSize(fonts.label));
        const std::string as = texts::T("match_setup.profile_page.rows.discord_as", {{"name", player.discord}});
        draw->AddCircleFilled({at.x + Dp(4), at.y + line * 0.5f}, Dp(4), Col(color::success));
        DrawLabel(draw, fonts.label, {at.x + Dp(16), at.y + (line - FontSize(fonts.label)) * 0.5f}, Col(color::text),
                  as.c_str());
        ImGui::Dummy({Dp(16) + TextSize(fonts.label, as.c_str()).x, line});
        if (status.info.Profiles())
        {
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos({at.x + std::max(profile_actions_, Dp(16) + TextSize(fonts.label, as.c_str()).x
                                                                            + Dp(18)),
                                       at.y + (line - Dp(32)) * 0.5f});
            const char* off = texts::T("match_setup.profile_page.rows.disconnect");
            if (GhostButton("discord_disconnect", off, {GhostWidth(off, Icon::Close), Dp(32)}, !status.busy,
                            Icon::Close))
                disconnect_prompt_ = true;
        }
        std::string friends;
        if (linked)
            friends = texts::T("match_setup.profile.linked",
                               {{"friends", texts::Count("match_setup.profile.friends", discord_status_.friends.size())},
                                {"players", std::to_string(status.discord_players.size())}});
        else if (waiting) friends = texts::T("match_setup.profile.connecting");
        else if (discord == DiscordState::Missing) friends = texts::T("match_setup.profile.not_in_build");
        else friends = texts::T("match_setup.profile.not_linked");
        TextColored(fonts.caption, linked || waiting || discord == DiscordState::Missing ? color::muted : color::warning,
                    friends.c_str());
        if (!linked && !waiting && discord != DiscordState::Missing)
        {
            ImGui::SameLine(0, Dp(10));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
            if (DiscordButton("link_discord_friends", texts::T("match_setup.profile.link"), Dp(96, 28)))
                discord_friends_->Connect();
        }
    }

    // Every frame: a rename that ended (the profile's Mii takes the new
    // name, its row closes), and an imported profile that went into this one
    // (its Mii is the profile's own now).
    void UpdateRenames()
    {
        const auto& status = competitive_status_;
        if (status.busy) return;
        if (!renaming_to_.empty() && status.profile_serial != rename_serial_)
        {
            if (status.player.name == renaming_to_)
            {
                profile_renaming_ = false;
                RenameOwnMii();
            }
            renaming_to_.clear();
        }
        if (taking_over_ && status.profile_serial != take_over_serial_)
        {
            const auto id = *taking_over_;
            taking_over_.reset();
            if (!status.profile_error.empty()) return;
            ChooseOnlineMii(platform::MiiCreateIdText(id));
            for (const auto& mii : InstalledMiis().miis)
                if (mii.create_id == id) competitive_->ChooseMii(mii.record);
            Notice(texts::T("match_setup.profile_page.imported.used", {{"name", status.player.name}}),
                   NoticeKind::Success);
        }
    }

    // The profile's Mii takes the profile's name (fitted to its 10 characters).
    void RenameOwnMii()
    {
        const auto own = PlayerOwnMii();
        if (!own || !own->here || own->record.size() != platform::mii::kMiiRecordBytes) return;
        const std::string name = platform::FitMiiName(competitive_status_.player.name);
        if (!platform::ValidMiiName(name) || platform::MiiRecordName(own->record) == name) return;
        auto record = own->record;
        try
        {
            platform::SetMiiRecordName(record, name);
            const auto folder = SaveFolder();
            platform::ReplaceMii(record, folder, SaveTitleOf(Release()), folder / "backups");
            ForgetInstalledMiis();
            if (competitive_status_.info.Profiles()) competitive_->ChooseMii(record);
        }
        catch (const std::exception& error)
        {
            Notice(texts::T("match_setup.editor.not_saved", {{"error", error.what()}}), NoticeKind::Error);
        }
    }

    // Connecting Discord found that the Discord account has a profile of its
    // own and the one signed in has played: put this one into it (its name,
    // face and friend code win, the sets of both stay), or not now.
    void DrawDiscordChoice()
    {
        constexpr const char* kTitle = "Discord has a profile";
        const auto& status = competitive_status_;
        if (status.discord_choice && status.discord_choice->takeover != discord_choice_seen_
            && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
            discord_choice_seen_ = status.discord_choice->takeover;
            ImGui::OpenPopup(kTitle);
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(480), 0}, {Dp(560), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                                        | ImGuiWindowFlags_NoMove))
        {
            if (!status.discord_choice) ImGui::CloseCurrentPopup();
            else
            {
                const auto& other = status.discord_choice->player;
                TextColored(fonts.heading, color::text, texts::T("match_setup.profile_page.choice.title"));
                ImGui::Dummy({0, Dp(2)});
                TextWrappedColored(fonts.body, color::muted,
                                   texts::T("match_setup.profile_page.choice.text",
                                            {{"other", other.name}, {"this", status.player.name}})
                                       .c_str());
                ImGui::Dummy({0, Dp(12)});
                if (PrimaryButton("choice_take_over", texts::T("match_setup.profile_page.choice.yes"), Dp(180, 42),
                                  !status.busy, Icon::Check))
                {
                    ImGui::CloseCurrentPopup();
                    competitive_->TakeOver();
                }
                ImGui::SameLine(0, Dp(10));
                if (SecondaryButton("choice_keep", texts::T("match_setup.profile_page.choice.no"), Dp(150, 42))
                    || CancelPressed())
                {
                    ImGui::CloseCurrentPopup();
                    competitive_->DropDiscordChoice();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // Import: the Miis that have been online in another save (Dolphin's, or
    // an old Recharged folder's) come into this computer's with their friend
    // codes (ImportProfiles).
    void DrawProfileImport()
    {
        constexpr const char* kTitle = "Import profiles";
        if (profile_import_prompt_)
        {
            ImGui::OpenPopup(kTitle);
            profile_import_prompt_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(480), 0}, {Dp(600), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                                        | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text, texts::T("match_setup.profile_page.import.title"));
            ImGui::Dummy({0, Dp(2)});
            TextWrappedColored(fonts.body, color::muted, texts::T("match_setup.profile_page.import.about"));
            ImGui::Dummy({0, Dp(10)});
            // Dolphin's user folder, when one has such Miis.
            fs::path dolphin;
            for (const auto& database : platform::FindDolphinMiiDatabases())
            {
                const auto user = database.parent_path().parent_path().parent_path().parent_path().parent_path();
                if (!platform::ResolveOnlineImport(user, Release().title_id).online.empty())
                {
                    dolphin = user;
                    break;
                }
            }
            bool browsing;
            {
                std::lock_guard<std::mutex> lock(folder_dialog_->mutex);
                browsing = folder_dialog_->active;
            }
            if (!dolphin.empty())
            {
                TextWrappedColored(fonts.caption, color::dim,
                                   texts::T("match_setup.wizard.found_in", {{"path", PathUtf8(dolphin)}}).c_str());
                ImGui::Dummy({0, Dp(4)});
                if (PrimaryButton("import_dolphin_profiles", texts::T("match_setup.profile_page.import.dolphin"),
                                  Dp(230, 42), config_ok_, Icon::Copy))
                {
                    ImGui::CloseCurrentPopup();
                    ImportProfiles(dolphin);
                }
                ImGui::SameLine(0, Dp(10));
            }
            if (SecondaryButton("import_choose_folder", texts::T("match_setup.wizard.choose_folder"), Dp(190, 42),
                                config_ok_ && !browsing, Icon::Folder))
            {
                ImGui::CloseCurrentPopup();
                BrowseFolder(FolderPurpose::ProfileImport);
            }
            ImGui::SameLine(0, Dp(10));
            if (GhostButton("import_profiles_cancel", texts::T("match_setup.buttons.cancel"), Dp(100, 42))
                || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // The Miis of `folder` that have been online, into this computer's save
    // with their slots. Signed out, the page then offers their profiles
    // (Welcome back); signed in, the player may take one of them
    // (DrawImportedProfiles).
    void ImportProfiles(const fs::path& folder)
    {
        const auto source = platform::ResolveOnlineImport(folder, Release().title_id);
        if (source.miis.empty())
        {
            Notice(texts::T("match_setup.profile_page.import.none", {{"folder", PathUtf8(folder)}}),
                   NoticeKind::Warning);
            return;
        }
        try
        {
            const auto save = SaveFolder();
            auto imported = platform::ImportOnlineMiis(source, save, SaveTitleOf(Release()), save / "backups");
            ForgetInstalledMiis();
            std::string names;
            for (const auto& mii : imported) names += (names.empty() ? "" : ", ") + mii.name;
            Notice(texts::T("match_setup.profile_page.import.done", {{"names", names}}), NoticeKind::Success);
            const auto& status = competitive_status_;
            if (status.state == CompetitiveService::State::SignedIn && status.info.Profiles())
            {
                imported_miis_ = std::move(imported);
                imported_prompt_ = true;
            }
        }
        catch (const std::exception& e)
        {
            Notice(texts::T("match_setup.import.failed", {{"error", e.what()}}), NoticeKind::Error);
        }
    }

    // Signed in, after an import: which imported Mii's profile goes into this
    // one (its name, face and friend code win, the sets of both stay), or none.
    void DrawImportedProfiles()
    {
        constexpr const char* kTitle = "Use an imported profile?";
        if (imported_prompt_)
        {
            ImGui::OpenPopup(kTitle);
            imported_prompt_ = false;
        }
        const auto& status = competitive_status_;
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(500), 0}, {Dp(620), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                                        | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text, texts::T("match_setup.profile_page.imported.title"));
            ImGui::Dummy({0, Dp(2)});
            TextWrappedColored(fonts.body, color::muted,
                               texts::T("match_setup.profile_page.imported.text", {{"name", status.player.name}})
                                   .c_str());
            ImGui::Dummy({0, Dp(10)});
            auto* draw = ImGui::GetWindowDrawList();
            const float width = Dp(440), height = Dp(54), figure = Dp(38), button = Dp(120);
            for (std::size_t n = 0; n < imported_miis_.size(); ++n)
            {
                const auto& mii = imported_miis_[n];
                const auto slot = platform::ReadOnlineSlot(mii.slot);
                if (!slot) continue;
                ImGui::PushID(int(n));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                DrawMii(draw, {at.x + figure * 0.5f, at.y + height * 0.5f}, figure, mii.favorite_color, mii.record);
                const float left = at.x + figure + Dp(12);
                DrawClipped(draw, fonts.label, {left, at.y + Dp(8)}, width - button - figure - Dp(24), color::text,
                            mii.name);
                DrawClipped(draw, fonts.caption, {left, at.y + Dp(8) + FontSize(fonts.label) + Dp(2)},
                            width - button - figure - Dp(24), color::muted,
                            texts::T("match_setup.mii.friend_code", {{"code", platform::FriendCodeText(mii.friend_code)}}));
                ImGui::SetCursorScreenPos({at.x + width - button, at.y + (height - Dp(36)) * 0.5f});
                if (SecondaryButton("use_imported", texts::T("match_setup.profile_page.imported.use"), {button, Dp(36)},
                                    !status.busy, Icon::Check))
                {
                    taking_over_ = mii.create_id;
                    take_over_serial_ = status.profile_serial;
                    competitive_->TakeOverSave(slot->user_id, slot->gsbrcd);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetCursorScreenPos(at);
                ImGui::Dummy({width, height});
                ImGui::PopID();
            }
            ImGui::Dummy({0, Dp(8)});
            if (GhostButton("imported_keep", texts::T("match_setup.profile_page.imported.keep"), Dp(160, 42))
                || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // Disconnect Discord, asked first: on other computers only Discord brings
    // a profile back.
    void DrawDisconnectPrompt()
    {
        constexpr const char* kTitle = "Disconnect Discord?";
        if (disconnect_prompt_)
        {
            ImGui::OpenPopup(kTitle);
            disconnect_prompt_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(460), 0}, {Dp(560), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                                        | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text, texts::T("match_setup.profile_page.disconnect.title"));
            ImGui::Dummy({0, Dp(2)});
            TextWrappedColored(fonts.body, color::muted, texts::T("match_setup.profile_page.disconnect.text"));
            ImGui::Dummy({0, Dp(12)});
            if (PrimaryButton("disconnect_confirm", texts::T("match_setup.profile_page.rows.disconnect"), Dp(160, 42),
                              true, Icon::Close))
            {
                ImGui::CloseCurrentPopup();
                competitive_->DisconnectDiscord();
                if (discord_friends_) discord_friends_->Disconnect();
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("disconnect_cancel", texts::T("match_setup.buttons.cancel"), Dp(100, 42))
                || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // Signing out here: the session, the Discord friends' link, and the Mii
    // chosen for it.
    void SignOutPlayer()
    {
        // Until the service is signed out, the player is not asked which Mii
        // plays for them (their chosen Mii goes now).
        signing_out_ = true;
        competitive_->SignOut();
        if (discord_friends_) discord_friends_->Disconnect();
        if (!draft_.online_mii.empty()) ChooseOnlineMii("");
    }

    // A Mii player's Sign out, asked first: no password brings them back,
    // their Mii logging in in the game does.
    void DrawSignOutPrompt()
    {
        if (sign_out_prompt_)
        {
            ImGui::OpenPopup("Sign out?");
            sign_out_prompt_ = false;
        }
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(460), 0}, {Dp(560), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Sign out?", nullptr, ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            const auto own = PlayerOwnMii();
            TextColored(fonts.heading, color::text, texts::T("match_setup.sign_out.title"));
            ImGui::Dummy({0, Dp(2)});
            // The game brings a Mii player back by the friend code of the Mii
            // that logged in; before its first online game there is none.
            if (competitive_status_.player.friend_code.empty())
                Callout(texts::T("match_setup.sign_out.no_friend_code"), color::warning, Icon::Warning);
            else
            {
                const std::string text = own && own->here
                    ? texts::T("match_setup.sign_out.come_back", {{"mii", own->name}})
                    : std::string(texts::T("match_setup.sign_out.come_back_your_mii"));
                TextWrappedColored(fonts.body, color::muted, text.c_str());
            }
            ImGui::Dummy({0, Dp(10)});
            if (PrimaryButton("sign_out_confirm", texts::T("match_setup.profile.sign_out"), Dp(140, 42), true,
                              Icon::Close))
            {
                ImGui::CloseCurrentPopup();
                SignOutPlayer();
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("sign_out_cancel", texts::T("match_setup.buttons.cancel"), Dp(100, 42))
                || CancelPressed())
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // A country's flag, `height` tall, its left edge at `at.x` and its middle
    // at `at.y`: how wide it is, 0 without one.
    float DrawFlag(ImDrawList* draw, ImVec2 at, float height, std::string_view country) const
    {
        const int cell = overlay::FlagCell(country);
        if (cell < 0 || !flags_) return 0.0f;
        const auto uv = overlay::FlagCellUv(cell);
        const float width = height * float(overlay::kFlagWidth) / float(overlay::kFlagHeight);
        const ImVec2 min{at.x, at.y - height * 0.5f}, max{at.x + width, at.y + height * 0.5f};
        draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(flags_)), min, max,
                              {uv.u0, uv.v0}, {uv.u1, uv.v1}, IM_COL32_WHITE, Dp(2));
        draw->AddRect(min, max, Col(color::text, 0.14f), Dp(2), 0, Dp(1));
        return width;
    }

    // The flag beside the player's name as a dropdown: Automatic (the
    // country found from where they connect), No flag, or a country; typing
    // a country's first letters goes to it.
    void FlagCombo(const platform::ProfilePlayer& player, bool busy)
    {
        const auto& fonts = CurrentFonts();
        const auto& countries = overlay::FlagCountries();
        const std::string detected(overlay::FlagName(player.country_detected));
        const std::string automatic = detected.empty()
            ? std::string(texts::T("match_setup.flag.automatic_none"))
            : texts::T("match_setup.flag.automatic", {{"country", detected}});
        const std::string& choice = player.country_choice;
        const std::string preview = choice == "auto" ? automatic
            : choice == "none"                      ? std::string(texts::T("match_setup.flag.none"))
                                                    : std::string(overlay::FlagName(choice));
        // A line: the flag (or the room for one), then the words.
        const float flag_h = Dp(16), flag_w = flag_h * float(overlay::kFlagWidth) / float(overlay::kFlagHeight);
        const auto line = [&](ImDrawList* draw, ImVec2 min, ImVec2 max, std::string_view country,
                              const std::string& text, bool muted) {
            const float middle = (min.y + max.y) * 0.5f;
            DrawFlag(draw, {min.x, middle}, flag_h, country);
            draw->PushClipRect(min, max, true);
            DrawLabel(draw, fonts.body, {min.x + flag_w + Dp(10), middle - FontSize(fonts.body) * 0.5f},
                      Col(muted ? color::muted : color::text), text.c_str());
            draw->PopClipRect();
        };
        const float row = Dp(34), width = std::min(ImGui::GetContentRegionAvail().x, Dp(360));
        ImGui::SetNextItemWidth(width);
        ImGui::BeginDisabled(busy);
        const ImGuiComboFlags flags = ImGuiComboFlags(ImGuiComboFlags_CustomPreview) | ImGuiComboFlags_HeightLarge;
        if (ImGui::BeginCombo("##flag", nullptr, flags))
        {
            std::optional<std::string> picked;
            const auto option = [&](const char* id, std::string_view key, std::string_view country,
                                    const std::string& text, bool muted, bool focus) {
                const bool selected = choice == key;
                if (focus) ImGui::SetKeyboardFocusHere();
                if (ImGui::Selectable(id, selected, 0, {0, row})) picked = std::string(key);
                if (selected && !focus) ImGui::SetItemDefaultFocus();
                line(ImGui::GetWindowDrawList(), {ImGui::GetItemRectMin().x + Dp(6), ImGui::GetItemRectMin().y},
                     {ImGui::GetItemRectMax().x - Dp(6), ImGui::GetItemRectMax().y}, country, text, muted);
            };
            // Typing goes to the first country whose name starts so.
            int typed = -1;
            if (ImGuiTypingSelectRequest* request = ImGui::GetTypingSelectRequest())
                typed = ImGui::TypingSelectFindMatch(
                    request, int(countries.size()),
                    [](void* list, int n) {
                        return (*static_cast<const std::vector<overlay::FlagCountry>*>(list))[std::size_t(n)].name;
                    },
                    const_cast<std::vector<overlay::FlagCountry>*>(&countries), -1);
            option("##auto", "auto", player.country_detected, automatic, false, false);
            option("##none", "none", {}, texts::T("match_setup.flag.none"), true, false);
            ImGui::Separator();
            for (std::size_t n = 0; n < countries.size(); ++n)
            {
                ImGui::PushID(int(n));
                option("##country", countries[n].code, countries[n].code, countries[n].name, false, int(n) == typed);
                ImGui::PopID();
            }
            ImGui::EndCombo();
            if (picked && *picked != choice) competitive_->ChooseCountry(*picked);
        }
        if (ImGui::BeginComboPreview())
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float top = at.y - (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f;
            line(ImGui::GetWindowDrawList(), {at.x, top}, {at.x + width - ImGui::GetFrameHeight() - Dp(16), top + ImGui::GetFrameHeight()},
                 choice == "auto" ? std::string_view(player.country_detected) : std::string_view(choice), preview,
                 choice == "none");
            ImGui::EndComboPreview();
        }
        ImGui::EndDisabled();
    }

    // A set's go, one value each time it goes: when it went (servers with
    // sets), else the set.
    static std::int64_t GoKey(const platform::ProfileMatch& match) { return match.go_at ? match.go_at : match.id; }

    // What the bar's pill says about the signed-in player: the set's stage.
    static std::pair<const char*, ImVec4> PlayerActivity(const std::optional<platform::ProfileMatch>& match)
    {
        if (!match || !match->Open()) return {texts::T("match_setup.activity.online"), color::success};
        if (match->state == "challenged")
            return match->side == "challenger" ? std::pair{texts::T("match_setup.activity.challenge_sent"), color::info}
                                               : std::pair{texts::T("match_setup.activity.challenged"), color::warning};
        if (match->stage == "go") return {texts::T("match_setup.activity.connecting"), color::accent};
        if (match->stage == "playing") return {texts::T("match_setup.activity.playing"), color::accent};
        if (match->stage == "open" && match->you_ready) return {texts::T("match_setup.activity.ready"), color::success};
        return {texts::T("match_setup.activity.getting_ready"), color::info};
    }

    // A test server's sign-in by name, for a second game on this computer
    // with its own settings file.
    void CompetitiveTestSignIn()
    {
        BeginCard("##competitive_test", texts::T("match_setup.test_player.title"),
                  texts::T("match_setup.test_player.about"), Icon::Person);
        const float button = Dp(120);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - Dp(10));
        const bool entered = ImGui::InputTextWithHint("##test_player", texts::T("match_setup.test_player.name_hint"),
                                                      &test_player_name_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, Dp(10));
        const bool usable = !competitive_status_.busy && !test_player_name_.empty();
        if (PrimaryButton("sign_in_test", texts::T("match_setup.test_player.sign_in"),
                          {button, ImGui::GetFrameHeight()}, usable, Icon::Person)
            || (entered && usable))
            competitive_->TestSignIn(test_player_name_);
        EndCard();
    }

    // The Miis of the save folder in use, read again when a file they come
    // from changes (an import, a new Mii, or the game writing its storage: a
    // Mii's first login puts its profile in the online save).
    // The game save's Miis. Asked several times a frame: the files' times are
    // looked at twice a second at most; the launcher's own writes read them
    // again at once (ForgetInstalledMiis).
    const mscharged::platform::MiiDatabaseStatus& InstalledMiis()
    {
        const auto folder = SaveFolder();
        const Uint64 now = SDL_GetTicks();
        if (folder == miis_folder_ && miis_checked_ && now - miis_checked_ < 500) return miis_;
        miis_checked_ = now;
        std::vector<fs::file_time_type> times;
        for (const auto& file : mscharged::platform::InstalledMiiFiles(folder, SaveTitleOf(Release())))
        {
            std::error_code error;
            const auto changed = fs::last_write_time(file, error);
            times.push_back(error ? fs::file_time_type{} : changed);
        }
        if (folder != miis_folder_ || times != miis_times_)
        {
            miis_folder_ = folder;
            miis_times_ = std::move(times);
            miis_ = mscharged::platform::InstalledMiis(folder, SaveTitleOf(Release()));
        }
        return miis_;
    }

    // After the launcher changed the Mii database (its write may not move the
    // file's time on a coarse file system): read it again at the next ask.
    void ForgetInstalledMiis()
    {
        miis_checked_ = 0;
        miis_times_.clear();
    }

    // The sidekicks each captain starts with, in the game save of the save
    // folder in use: the game's sidekick screen shows them, and what is
    // chosen there comes back here.
    void SidekicksPage()
    {
        PageHeader(texts::T("match_setup.sidekicks.title"), texts::T("match_setup.sidekicks.about"));
        using mscharged::platform::GameSaveState;
        const auto& save = InstalledLineups();
        GameSaveCallout(save.state, save.error, texts::T("match_setup.sidekicks.save_missing"),
                        texts::T("match_setup.sidekicks.save_damaged"));
        const bool ready = save.state == GameSaveState::Ready;
        const auto lineups = ready ? save.lineups : mscharged::platform::DefaultSidekickLineups();

        ImGui::BeginDisabled(!ready);
        BeginCard("##captains", texts::T("match_setup.sidekicks.captains"),
                  texts::T("match_setup.sidekicks.captains_about"), Icon::Person);
        CaptainGrid(lineups);
        EndCard();

        const auto& lineup = lineups[std::size_t(lineup_captain_)];
        const auto standard = mscharged::platform::DefaultSidekickLineups()[std::size_t(lineup_captain_)];
        BeginCard("##lineup", kCaptainNames[lineup_captain_], texts::T("match_setup.sidekicks.lineup_about"),
                  Icon::Pitch);
        Pitch(lineups);
        if (BeginRows("##lineup_rows"))
        {
            const std::string description = texts::T(
                "match_setup.sidekicks.standard_about",
                {{"sidekicks", SidekickList(standard)}, {"captain", kCaptainNames[lineup_captain_]}});
            Row(texts::T("match_setup.sidekicks.standard"), description.c_str());
            if (SecondaryButton("restore_lineup", texts::T("match_setup.sidekicks.restore"),
                                {ImGui::GetContentRegionAvail().x, Dp(40)}, ready && lineup != standard, Icon::Refresh))
            {
                auto changed = lineups;
                changed[std::size_t(lineup_captain_)] = standard;
                SaveLineups(changed);
            }
            EndRows();
        }
        EndCard();
        ImGui::EndDisabled();
    }

    // The twelve captains, each with the sidekicks that start with them; a
    // dot marks a lineup that is not the game's own.
    void CaptainGrid(const mscharged::platform::SidekickLineups& lineups)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const auto standard = mscharged::platform::DefaultSidekickLineups();
        constexpr int columns = 4;
        const float gap = Dp(10), width = (ImGui::GetContentRegionAvail().x - gap * (columns - 1)) / columns,
                    height = Dp(60);
        for (int captain = 0; captain < int(lineups.size()); ++captain)
        {
            if (captain % columns) ImGui::SameLine(0, gap);
            ImGui::PushID(captain);
            const ImVec2 min = ImGui::GetCursorScreenPos(), max{min.x + width, min.y + height};
            if (ImGui::InvisibleButton("##captain", {width, height})) lineup_captain_ = captain;
            const bool selected = captain == lineup_captain_;
            const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 14.0f);
            const float active = Animate(ImGui::GetID("##selected"), selected, 12.0f);
            draw->AddRectFilled(min, max, Col(Mix(Mix(color::field, color::card_hover, hover), color::accent_dark, active)),
                                Dp(10));
            draw->AddRect(min, max, Col(Mix(color::border, color::accent, active), 0.6f + 0.4f * active), Dp(10), 0,
                          Dp(1.0f + 0.5f * active));
            draw->PushClipRect({min.x + Dp(12), min.y}, {max.x - Dp(16), max.y}, true);
            DrawLabel(draw, fonts.label, {min.x + Dp(12), min.y + Dp(10)},
                      Col(Mix(color::text, color::accent_hi, active)), kCaptainNames[captain]);
            const auto& lineup = lineups[std::size_t(captain)];
            std::string sidekicks;
            for (const int sidekick : lineup) sidekicks += (sidekicks.empty() ? "" : " - ") + Initials(sidekick);
            DrawLabel(draw, fonts.caption, {min.x + Dp(12), min.y + Dp(34)}, Col(color::muted), sidekicks.c_str());
            draw->PopClipRect();
            if (lineup != standard[std::size_t(captain)])
                draw->AddCircleFilled({max.x - Dp(12), min.y + Dp(12)}, Dp(3.5f), Col(color::accent));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", SidekickList(lineup).c_str());
            ImGui::PopID();
        }
    }

    // The chosen captain's team on a pitch, attacking to the right; each
    // sidekick opens a menu of the eight.
    void Pitch(const mscharged::platform::SidekickLineups& lineups)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x, height = std::round(width * 0.5f);
        const ImVec2 min = ImGui::GetCursorScreenPos(), max{min.x + width, min.y + height};
        ImGui::Dummy({width, height});
        const ImVec2 after = ImGui::GetCursorScreenPos();

        // The grass, mown in stripes, and its markings.
        FillGradient(draw, min, max, Col(Mix(color::accent_dark, color::card, 0.25f)), Col(color::accent_dark), Dp(14),
                     false);
        const ImU32 line = Col(color::text, 0.24f);
        const float thick = Dp(1.5f), inset = Dp(16);
        const ImVec2 field_min{min.x + inset, min.y + inset}, field_max{max.x - inset, max.y - inset};
        const float field_w = field_max.x - field_min.x, field_h = field_max.y - field_min.y;
        for (int stripe = 1; stripe < 12; stripe += 2)
            draw->AddRectFilled({field_min.x + field_w * float(stripe) / 12, field_min.y},
                                {field_min.x + field_w * float(stripe + 1) / 12, field_max.y}, Col(color::text, 0.025f));
        draw->AddRect(field_min, field_max, line, Dp(4), 0, thick);
        const float middle = (field_min.x + field_max.x) * 0.5f, centre = (field_min.y + field_max.y) * 0.5f;
        draw->AddLine({middle, field_min.y}, {middle, field_max.y}, line, thick);
        // AddLine and AddRect draw half a pixel right and down (pixel centres); the circles follow them.
        draw->AddCircle({middle + 0.5f, centre + 0.5f}, field_h * 0.17f, line, 0, thick);
        draw->AddCircleFilled({middle + 0.5f, centre + 0.5f}, Dp(3), line);
        const float box_w = field_w * 0.09f, box_h = field_h * 0.46f, goal_h = field_h * 0.2f, goal_d = Dp(6);
        draw->AddRect({field_min.x, centre - box_h * 0.5f}, {field_min.x + box_w, centre + box_h * 0.5f}, line, 0, 0, thick);
        draw->AddRect({field_max.x - box_w, centre - box_h * 0.5f}, {field_max.x, centre + box_h * 0.5f}, line, 0, 0, thick);
        draw->AddRectFilled({field_min.x - goal_d, centre - goal_h * 0.5f}, {field_min.x, centre + goal_h * 0.5f}, line);
        draw->AddRectFilled({field_max.x, centre - goal_h * 0.5f}, {field_max.x + goal_d, centre + goal_h * 0.5f}, line);
        const char* attack = texts::T("match_setup.sidekicks.attack");
        const float attack_w = TrackedWidth(fonts.caption, attack, Dp(1.6f));
        const ImVec2 attack_at{field_max.x - box_w - Dp(34) - attack_w, field_min.y + Dp(10)};
        DrawTracked(draw, fonts.caption, attack_at, Col(color::text, 0.4f), attack, Dp(1.6f));
        DrawChevron(draw, {attack_at.x + attack_w + Dp(12), attack_at.y + FontSize(fonts.caption) * 0.5f}, Dp(11),
                    Col(color::text, 0.4f));

        // The players: a disc with the name's initials, the role above it and the name below.
        const float radius = std::clamp(field_h * 0.09f, Dp(18), Dp(30));
        auto spot = [&](const PitchSpot& at) {
            return ImVec2{field_min.x + field_w * at.x, field_min.y + field_h * at.y};
        };
        auto player = [&](const PitchSpot& at, const char* name, const std::string& initials, const ImVec4& tint,
                          float hover) {
            const ImVec2 c = spot(at);
            draw->AddCircleFilled(c, radius + Dp(4) * hover, Col(color::window, 0.85f));
            draw->AddCircleFilled(c, radius, Col(Mix(color::card, tint, 0.18f + 0.14f * hover)));
            draw->AddCircle(c, radius, Col(tint, 0.75f + 0.25f * hover), 0, Dp(2.0f + hover));
            // Centred on the capitals' middle: the line's height also holds the room below the baseline.
            const ImVec2 letters = TextSize(fonts.heading, initials.c_str());
            const ImFontGlyph* capital = fonts.heading->FindGlyph('H');
            const float middle = capital ? (capital->Y0 + capital->Y1) * 0.5f * ImGui::GetIO().FontGlobalScale
                                         : letters.y * 0.5f;
            DrawLabel(draw, fonts.heading, {c.x - letters.x * 0.5f, c.y - middle}, Col(color::text), initials.c_str());
            // The role on a dark tag, readable over the pitch's markings.
            const float role_w = TrackedWidth(fonts.caption, at.role, Dp(1.4f)), role_h = FontSize(fonts.caption);
            const ImVec2 role{c.x - role_w * 0.5f, c.y - radius - Dp(8) - role_h};
            draw->AddRectFilled({role.x - Dp(6), role.y - Dp(2)}, {role.x + role_w + Dp(5), role.y + role_h + Dp(2)},
                                Col(color::window, 0.6f), Dp(4));
            DrawTracked(draw, fonts.caption, role, Col(tint, 0.9f), at.role, Dp(1.4f));
            const ImVec2 label = TextSize(fonts.label, name);
            DrawLabelShadowed(draw, fonts.label, {c.x - label.x * 0.5f, c.y + radius + Dp(5)}, Col(color::text), name,
                              Dp(1.5f));
        };
        player(kGoalieSpot, "Kritter", "Kr", color::muted, 0.0f);
        const std::string captain = kCaptainNames[lineup_captain_];
        player(kCaptainSpot, captain.c_str(), NameInitials(captain), color::accent_hi, 0.0f);
        const auto& lineup = lineups[std::size_t(lineup_captain_)];
        for (std::size_t slot = 0; slot < lineup.size(); ++slot)
        {
            const ImVec2 c = spot(kSidekickSpots[slot]);
            ImGui::PushID(int(slot));
            ImGui::SetCursorScreenPos({c.x - radius, c.y - radius});
            if (ImGui::InvisibleButton("##sidekick", {radius * 2, radius * 2}))
            {
                lineup_slot_ = int(slot);
                ImGui::OpenPopup("##sidekick_menu");
            }
            const bool hovered = ImGui::IsItemHovered();
            const float hover = Animate(ImGui::GetItemID(), hovered || (lineup_slot_ == int(slot)
                                        && ImGui::IsPopupOpen("##sidekick_menu")), 14.0f);
            player(kSidekickSpots[slot], kSidekickNames[lineup[slot]], Initials(lineup[slot]), color::info, hover);
            if (hovered)
                ImGui::SetTooltip("%s", texts::T("match_setup.sidekicks.choose_another",
                                                 {{"role", Lowercase(kSidekickSpots[slot].role)}})
                                            .c_str());
            SidekickMenu(lineups, slot);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy({0, Dp(6)});
    }

    // The eight sidekicks for one slot of the chosen captain, the one there ticked.
    void SidekickMenu(const mscharged::platform::SidekickLineups& lineups, std::size_t slot)
    {
        const auto& fonts = CurrentFonts();
        const float width = Dp(220);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, Dp(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(10));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(color::card, color::window, 0.25f));
        if (ImGui::BeginPopup("##sidekick_menu"))
        {
            const int current = lineups[std::size_t(lineup_captain_)][slot];
            auto* draw = ImGui::GetWindowDrawList();
            for (int sidekick = 0; sidekick < int(std::size(kSidekickNames)); ++sidekick)
            {
                ImGui::PushID(sidekick);
                const ImVec2 pos = ImGui::GetCursorScreenPos(), size{width, Dp(36)};
                const bool pressed = ImGui::InvisibleButton("##row", size);
                const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 14.0f);
                if (hover > 0.0f)
                    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, Col(color::text, 0.06f * hover), Dp(7));
                const bool chosen = sidekick == current;
                const ImU32 ink = Col(chosen ? color::accent : Mix(Mix(color::muted, color::text, 0.7f), color::text, hover));
                DrawLabel(draw, fonts.label, {pos.x + Dp(12), pos.y + (size.y - FontSize(fonts.label)) * 0.5f}, ink,
                          kSidekickNames[sidekick]);
                if (chosen) DrawIcon(draw, Icon::Check, {pos.x + width - Dp(18), pos.y + size.y * 0.5f}, Dp(14), ink);
                if (pressed)
                {
                    if (!chosen)
                    {
                        auto changed = lineups;
                        changed[std::size_t(lineup_captain_)][slot] = sidekick;
                        SaveLineups(changed);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    // Write the lineups into the game save.
    void SaveLineups(const mscharged::platform::SidekickLineups& lineups)
    {
        const auto& lineup = lineups[std::size_t(lineup_captain_)];
        fs::path copy;
        try
        {
            copy = mscharged::platform::WriteSidekicks(SaveFolder(), SaveTitleOf(Release()), lineups, SaveBackupParent());
            Notice(texts::T("match_setup.sidekicks.saved",
                            {{"captain", kCaptainNames[lineup_captain_]}, {"sidekicks", SidekickList(lineup)}}),
                   NoticeKind::Success);
        }
        catch (const std::exception& e)
        {
            Notice(texts::T("match_setup.sidekicks.save_failed", {{"error", e.what()}}), NoticeKind::Error);
        }
        GameSaveWritten(copy);
    }

    // What the Teams and Matches pages say when the game save cannot be changed:
    // `missing` for a save that is not there yet, `damaged` for one the game
    // cannot read (each saying what they would change).
    static void GameSaveCallout(mscharged::platform::GameSaveState state, const std::string& error, const char* missing,
                                const char* damaged)
    {
        using mscharged::platform::GameSaveState;
        if (state == GameSaveState::Missing) Callout(missing, color::info, Icon::Info);
        else if (state == GameSaveState::Damaged) Callout(damaged, color::warning, Icon::Warning);
        else if (state == GameSaveState::Unusable)
            Callout(error, color::danger, Icon::Error);
    }

    // Where the Teams and Matches pages back the game save up before they
    // change it: the first change of a session in a save folder copies the
    // save to its backups/, the next ones follow it (none: empty).
    fs::path SaveBackupParent() const
    {
        const auto folder = SaveFolder();
        return save_backed_up_ == folder ? fs::path() : folder / "backups";
    }

    // After a change to the game save: `copy` is its backup, if one was made.
    // Both pages read the save again.
    void GameSaveWritten(const fs::path& copy)
    {
        if (!copy.empty()) save_backed_up_ = SaveFolder();
        lineups_folder_.clear();
    }

    // The lineups of the save folder in use, read again when it or its
    // catalog changes (an import), or after the launcher writes them. The
    // game changes them only while the launcher is closed.
    const mscharged::platform::SidekickSaveStatus& InstalledLineups()
    {
        const auto folder = SaveFolder();
        std::error_code error;
        const auto changed = fs::last_write_time(folder / "metadata.txt", error);
        const auto time = error ? fs::file_time_type{} : changed;
        if (folder != lineups_folder_ || time != lineups_time_)
        {
            lineups_folder_ = folder;
            lineups_time_ = time;
            lineups_ = mscharged::platform::InstalledSidekicks(folder, SaveTitleOf(Release()));
        }
        return lineups_;
    }

    static std::string SidekickList(const mscharged::platform::SidekickLineup& lineup)
    {
        return texts::T("match_setup.sidekicks.list", {{"first", kSidekickNames[lineup[0]]},
                                                        {"second", kSidekickNames[lineup[1]]},
                                                        {"third", kSidekickNames[lineup[2]]}});
    }

    // "Shy Guy" SG, "Toad" To.
    static std::string NameInitials(const std::string& name)
    {
        std::string initials;
        for (std::size_t n = 0; n < name.size(); ++n)
            if (std::isupper(static_cast<unsigned char>(name[n])) && (n == 0 || name[n - 1] == ' ')) initials += name[n];
        if (initials.size() == 1 && name.size() > 1) initials += name[1];
        return initials;
    }

    static std::string Initials(int sidekick) { return NameInitials(kSidekickNames[sidekick]); }

    static std::string Lowercase(std::string text)
    {
        for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // Match rules: presets of a battle's rules, which a challenge or a search
    // for an opponent starts from (the game's online panel, F1). Retail ranked
    // and Competitive are built in, the server may offer rule sets of its own,
    // and the player's are files in match-presets beside the settings file.
    void MatchRulesPage()
    {
        const auto& fonts = CurrentFonts();
        {
            // The title, and New preset at its right.
            const char* label = texts::T("match_setup.rules.new_preset");
            const float button_w = GhostWidth(label, Icon::Copy) + Dp(24), button_h = Dp(40);
            const ImVec2 top = ImGui::GetCursorScreenPos();
            const float right = ImGui::GetContentRegionAvail().x;
            // As PageHeader, its text beside the button.
            TextColored(fonts.title, color::text, texts::T("match_setup.rules.title"));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(4));
            ImGui::PushFont(fonts.body);
            ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + right - button_w - Dp(24));
            ImGui::TextUnformatted(texts::T("match_setup.rules.about", {{"key", "F1"}}).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Dummy({0, Dp(8)});
            const ImVec2 after = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({top.x + right - button_w, top.y + Dp(2)});
            if (PrimaryButton("new_match_preset", label, {button_w, button_h}, true, Icon::Copy))
            {
                launcher::MatchPreset fresh = launcher::BuiltInMatchPresets()[0];
                fresh.name.clear();
                fresh.built_in = false;
                EditPreset(fresh);
            }
            ImGui::SetCursorScreenPos(after);
        }
        const auto& list = MatchPresets();
        BeginCard("##match_presets", nullptr);

        PresetGroupTitle(texts::T("match_setup.rules.groups.built_in"));
        int row = 0;
        for (const auto& preset : list.presets)
            if (preset.built_in)
            {
                if (PresetListRow(row++, preset.name, launcher::ChallengeRules(preset), PresetKind::BuiltIn))
                    CopyPreset(preset.name, launcher::ChallengeRules(preset));
            }

        // The server's own rule sets, besides those named as the built-in ones.
        std::vector<const platform::RuleSet*> from_server;
        if (competitive_status_.state == CompetitiveService::State::SignedIn)
            for (const auto& set : competitive_status_.rule_sets)
                if (std::none_of(list.presets.begin(), list.presets.end(), [&](const launcher::MatchPreset& preset) {
                        return preset.built_in && preset.name == set.name;
                    }))
                    from_server.push_back(&set);
        if (!from_server.empty())
        {
            PresetGroupTitle(texts::T("match_setup.rules.groups.server"));
            for (const auto* set : from_server)
                if (PresetListRow(row++, set->name, set->rules, PresetKind::Server)) CopyPreset(set->name, set->rules);
        }

        PresetGroupTitle(texts::T("match_setup.rules.groups.yours"));
        bool own = false;
        for (std::size_t n = 0; n < list.presets.size(); ++n)
        {
            const auto& preset = list.presets[n];
            if (preset.built_in) continue;
            own = true;
            const int id = row++;
            switch (PresetListRow(id, preset.name, launcher::ChallengeRules(preset), PresetKind::Own, int(n)))
            {
            case 1: CopyPreset(preset.name, launcher::ChallengeRules(preset)); break;
            case 2: EditPreset(preset); break;
            case 3:
                try
                {
                    launcher::DeleteMatchPreset(preset);
                    Notice(texts::T("match_setup.rules.deleted", {{"name", preset.name}}), NoticeKind::Success);
                }
                catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); }
                preset_delete_ = -1;
                presets_loaded_ = false;
                break;
            default: break;
            }
        }
        if (!own)
            TextWrappedColored(fonts.caption, color::muted, texts::T("match_setup.rules.none_yet"));
        const std::string folder = texts::T("match_setup.rules.folder", {{"path", PathUtf8(MatchPresetsFolder())}});
        ImGui::Dummy({0, Dp(4)});
        TextWrappedColored(fonts.caption, color::dim, folder.c_str());
        for (const auto& skipped : list.skipped)
            TextWrappedColored(fonts.caption, color::warning,
                               texts::T("match_setup.rules.skipped", {{"file", skipped}}).c_str());
        EndCard();
    }

    enum class PresetKind
    {
        BuiltIn,
        Server,
        Own,
    };

    // A group's name in the presets list, in small tracked capitals.
    void PresetGroupTitle(const char* title)
    {
        const auto& fonts = CurrentFonts();
        ImGui::Dummy({0, Dp(4)});
        DrawTracked(ImGui::GetWindowDrawList(), fonts.caption, ImGui::GetCursorScreenPos(), Col(color::dim), title,
                    Dp(1.4f));
        ImGui::Dummy({0, FontSize(fonts.caption) + Dp(6)});
    }

    // The rules as short chips: the series, how a game ends, the stadium, the
    // gameplay tweaks and the cheats.
    struct RuleChip
    {
        std::string text;
        ImVec4 tint;
        bool plain = true; // white text; else in its tint
    };
    static std::vector<RuleChip> RuleChips(const platform::MatchRules& rules)
    {
        std::vector<RuleChip> chips;
        chips.push_back({rules.games == 1
                             ? std::string(texts::T("match_setup.rules.one_game"))
                             : texts::T("match_setup.rules.best_of", {{"games", std::to_string(rules.games)}}),
                         color::muted});
        if (rules.limit == "goals")
            chips.push_back({texts::Count("match_setup.rules.first_to_goals", rules.goals), color::muted});
        else
            chips.push_back({rules.time % 60 == 0 ? texts::Count("match_setup.rules.minutes", rules.time / 60)
                                                  : texts::Count("match_setup.rules.seconds", rules.time),
                             color::muted});
        std::string stadium = texts::T("match_setup.rules.random_stadium");
        for (const auto& choice : platform::StadiumChoices())
            if (rules.stadium != "random" && rules.stadium == choice.key) stadium = choice.label;
        chips.push_back({stadium, color::muted});
        for (const auto& choice : platform::RuleTweakChoices())
            if (std::find(rules.tweaks.begin(), rules.tweaks.end(), choice.key) != rules.tweaks.end())
                chips.push_back({choice.label, color::accent, false});
        for (const auto& [choices, key] : {std::pair{&platform::StadiumCheatChoices(), &rules.stadium_cheat},
                                           std::pair{&platform::PlayerCheatChoices(), &rules.player_cheat},
                                           std::pair{&platform::PowerupCheatChoices(), &rules.powerup_cheat}})
            for (const auto& choice : *choices)
                if (*key != "none" && *key == choice.key) chips.push_back({choice.label, color::warning, false});
        return chips;
    }

    // One preset of the list: its name (and where it comes from), its rules
    // as chips, wrapping, and its buttons at the right: Copy, and for the
    // player's own Edit and Delete (pressed twice). Returns the button pressed:
    // 1 Copy, 2 Edit, 3 Delete (confirmed), 0 none.
    int PresetListRow(int id, const std::string& name, const platform::MatchRules& rules, PresetKind kind,
                      int index = -1)
    {
        const auto& fonts = CurrentFonts();
        auto* draw = ImGui::GetWindowDrawList();
        ImGui::PushID(id);
        const float width = ImGui::GetContentRegionAvail().x, pad = Dp(10), gap = Dp(6), height = Dp(30);
        const bool own = kind == PresetKind::Own;
        const float copy_w = Dp(86), edit_w = Dp(80), delete_w = Dp(96);
        const float buttons = copy_w + (own ? gap + edit_w + gap + delete_w : 0.0f);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        // The name, and for rules that stay as they are a padlock: they come
        // with Recharged or from the server, and a copy is the player's.
        const float text_right = start.x + width - pad - buttons - Dp(16);
        ImGui::SetCursorScreenPos({start.x + pad + Dp(4), start.y + pad});
        TextColored(fonts.label, color::text, name.c_str());
        if (!own)
        {
            const char* tag = kind == PresetKind::BuiltIn ? texts::T("match_setup.rules.fixed")
                                                          : texts::T("match_setup.rules.from_server");
            const ImVec2 size = TextSize(fonts.caption, tag);
            const float tag_h = FontSize(fonts.caption) + Dp(6);
            const ImVec2 tag_at{start.x + pad + Dp(4) + TextSize(fonts.label, name.c_str()).x + Dp(10),
                                start.y + pad + (FontSize(fonts.label) - tag_h) * 0.5f};
            const ImVec2 tag_end{tag_at.x + Dp(22) + size.x + Dp(8), tag_at.y + tag_h};
            draw->AddRectFilled(tag_at, tag_end, Col(color::text, 0.06f), tag_h * 0.5f);
            DrawIcon(draw, Icon::Lock, {tag_at.x + Dp(12), tag_at.y + tag_h * 0.5f}, Dp(11), Col(color::muted));
            DrawLabel(draw, fonts.caption, {tag_at.x + Dp(22), tag_at.y + (tag_h - size.y) * 0.5f}, Col(color::muted),
                      tag);
            if (ImGui::IsMouseHoveringRect(tag_at, tag_end))
                ImGui::SetTooltip("%s", kind == PresetKind::BuiltIn ? texts::T("match_setup.rules.fixed_tooltip")
                                                                    : texts::T("match_setup.rules.server_tooltip"));
        }
        // The rules, as chips that wrap.
        const auto chips = RuleChips(rules);
        float x = start.x + pad + Dp(4), y = start.y + pad + FontSize(fonts.label) + Dp(6);
        const float chip_h = FontSize(fonts.caption) + Dp(8);
        for (const auto& [text, tint, plain] : chips)
        {
            const float w = TextSize(fonts.caption, text.c_str()).x + Dp(16);
            if (x + w > text_right && x > start.x + pad + Dp(4))
            {
                x = start.x + pad + Dp(4);
                y += chip_h + Dp(4);
            }
            draw->AddRectFilled({x, y}, {x + w, y + chip_h}, Col(tint, 0.12f), chip_h * 0.5f);
            draw->AddRect({x, y}, {x + w, y + chip_h}, Col(tint, 0.35f), chip_h * 0.5f, 0, Dp(1));
            DrawLabel(draw, fonts.caption, {x + Dp(8), y + Dp(4)}, Col(plain ? color::text : tint), text.c_str());
            x += w + Dp(6);
        }
        const float bottom = std::max(y + chip_h, start.y + pad + height) + pad;
        // The buttons, in the middle at the right.
        int pressed = 0;
        float bx = start.x + width - pad - buttons;
        const float by = start.y + (bottom - start.y - height) * 0.5f;
        ImGui::SetCursorScreenPos({bx, by});
        if (SecondaryButton("copy", texts::T("match_setup.buttons.copy"), {copy_w, height}, !preset_edit_, Icon::Copy))
            pressed = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", texts::T("match_setup.rules.copy_tooltip", {{"name", name}}).c_str());
        if (own)
        {
            bx += copy_w + gap;
            ImGui::SetCursorScreenPos({bx, by});
            if (SecondaryButton("edit", texts::T("match_setup.buttons.edit"), {edit_w, height}, !preset_edit_,
                                Icon::Paint))
                pressed = 2;
            bx += edit_w + gap;
            ImGui::SetCursorScreenPos({bx, by});
            const bool armed = preset_delete_ == index;
            const char* delete_label = armed ? texts::T("match_setup.buttons.confirm")
                                             : texts::T("match_setup.buttons.delete");
            if (SecondaryButton("delete", delete_label, {delete_w, height}, !preset_edit_,
                                armed ? Icon::Warning : Icon::Close))
            {
                if (armed) pressed = 3;
                else preset_delete_ = index;
            }
            if (armed && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("match_setup.rules.delete_tooltip"));
        }
        // The row's card, under it all.
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(start, {start.x + width, bottom}, Col(color::field, 0.55f), Dp(12));
        draw->AddRect(start, {start.x + width, bottom}, Col(color::border, 0.9f), Dp(12), 0, Dp(1));
        draw->ChannelsMerge();
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy({width, bottom - start.y});
        ImGui::PopID();
        return pressed;
    }

    // A preset of the player's own from `name`'s rules, in the editor.
    void CopyPreset(const std::string& name, const platform::MatchRules& rules)
    {
        EditPreset(launcher::PresetFromRules(texts::T("match_setup.rules.copy_name", {{"name", name}}), rules));
    }

    // The editor opens at its top, wherever the list was scrolled to.
    void EditPreset(const launcher::MatchPreset& preset)
    {
        preset_edit_ = preset;
        preset_delete_ = -1;
        page_scroll_top_ = true;
    }

    // The preset being made or changed, on a page of its own (as the Mii
    // editor): its name, the settings of the game's settings screen and the
    // cheats, all offered whether unlocked or not; back or Cancel leave it,
    // Save keeps it.
    void PresetEditorPage()
    {
        auto& preset = *preset_edit_;
        auto& s = preset.settings;
        const auto& fonts = CurrentFonts();
        const bool fresh = preset.file.empty();
        if (EditorHeader("##preset_editor_back",
                         fresh ? texts::T("match_setup.rules.new_preset") : texts::T("match_setup.preset_editor.title"),
                         texts::T("match_setup.preset_editor.about", {{"folder", "match-presets"}}).c_str()))
        {
            preset_edit_.reset();
            page_scroll_top_ = true;
            return;
        }
        BeginCard("##preset_editor", nullptr);
        // Why the preset cannot be saved, if it cannot.
        std::string blocked;
        if (!launcher::ValidMatchPresetName(preset.name))
            blocked = texts::T("match_setup.preset_editor.name_rule",
                               {{"retail", launcher::BuiltInMatchPresets()[0].name},
                                {"competitive", launcher::BuiltInMatchPresets()[1].name}});
        for (const auto& other : MatchPresets().presets)
            if (!other.built_in && other.file != preset.file && launcher::SameMatchPresetName(other.name, preset.name))
                blocked = texts::T("match_setup.presets.name_taken", {{"name", other.name}});
        if (BeginRows("##preset_rows"))
        {
            Row(texts::T("match_setup.preset_editor.name"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##preset_name", texts::T("match_setup.preset_editor.name_hint"), &preset.name);
            if (!blocked.empty() && !preset.name.empty())
                TextWrappedColored(fonts.caption, color::warning, blocked.c_str());

            Row(texts::T("match_setup.preset_editor.series"), texts::T("match_setup.preset_editor.series_about"));
            int series = std::min((s.best_of - 1) / 2, 3);
            if (Segmented("##preset_series", &series, {"1", "3", "5", "7"}, ImGui::GetContentRegionAvail().x))
                s.best_of = series * 2 + 1;

            Row(texts::T("match_setup.preset_editor.win_by"), s.goal_limit
                                                                ? texts::T("match_setup.preset_editor.win_by_goals")
                                                                : texts::T("match_setup.preset_editor.win_by_time"));
            int limit = s.goal_limit ? 1 : 0;
            if (Segmented("##preset_limit", &limit,
                          {texts::T("match_setup.preset_editor.time"), texts::T("match_setup.preset_editor.goals")},
                          ImGui::GetContentRegionAvail().x))
                s.goal_limit = limit == 1;
            if (s.goal_limit)
            {
                Row(texts::T("match_setup.preset_editor.goals"));
                int goals = s.goals - 3;
                if (Segmented("##preset_goals", &goals, {"3", "4", "5", "6", "7", "8", "9", "10"},
                              ImGui::GetContentRegionAvail().x))
                    s.goals = goals + 3;
            }
            else
            {
                Row(texts::T("match_setup.preset_editor.time"));
                int time = s.time / 60 - 2;
                std::vector<std::string> minutes;
                for (int n = 2; n <= 5; ++n)
                    minutes.push_back(
                        texts::T("match_setup.preset_editor.minutes_short", {{"minutes", std::to_string(n)}}));
                if (Segmented("##preset_time", &time,
                              {minutes[0].c_str(), minutes[1].c_str(), minutes[2].c_str(), minutes[3].c_str()},
                              ImGui::GetContentRegionAvail().x))
                    s.time = (time + 2) * 60;
            }

            Row(texts::T("match_setup.preset_editor.stadium_cheat"));
            CheatCombo("##preset_stadium", s.stadium_cheat, launcher::StadiumCheatNames());
            Row(texts::T("match_setup.preset_editor.player_cheat"));
            CheatCombo("##preset_player", s.player_cheat, launcher::PlayerCheatNames());
            Row(texts::T("match_setup.preset_editor.powerup_cheat"));
            CheatCombo("##preset_powerup", s.powerup_cheat, launcher::PowerupCheatNames());

            // For challenges: the stadium and the gameplay tweaks both teams play with.
            Row(texts::T("match_setup.preset_editor.stadium"), texts::T("match_setup.preset_editor.stadium_about"));
            {
                const auto& stadiums = platform::StadiumChoices();
                std::vector<const char*> names;
                int chosen = 0;
                for (std::size_t n = 0; n < stadiums.size(); ++n)
                {
                    names.push_back(stadiums[n].label);
                    if (preset.stadium == stadiums[n].key) chosen = int(n);
                }
                CheatCombo("##preset_stadium_pick", chosen, names);
                preset.stadium = stadiums[std::size_t(chosen)].key;
            }
            Row(texts::T("match_setup.preset_editor.gameplay"), texts::T("match_setup.preset_editor.gameplay_about"));
            for (const auto& choice : platform::RuleTweakChoices())
            {
                const auto found = std::find(preset.tweaks.begin(), preset.tweaks.end(), choice.key);
                bool on = found != preset.tweaks.end();
                ImGui::PushID(choice.key);
                if (Toggle("##preset_tweak", &on))
                {
                    if (on) preset.tweaks.push_back(choice.key);
                    else preset.tweaks.erase(found);
                }
                ImGui::SameLine(0, Dp(12));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Dp(28) - FontSize(fonts.body)) * 0.5f);
                TextColored(fonts.body, color::text, choice.label);
                ImGui::PopID();
            }
            EndRows();
        }
        // Cancel and Save at the right, as in the Mii editor; why it can't be
        // saved before them.
        ImGui::Dummy({0, Dp(14)});
        const float button = Dp(36), end_button = Dp(124);
        const ImVec2 bar = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float right_start = bar.x + width - end_button * 2 - Dp(8);
        if (!blocked.empty() && preset.name.empty())
        {
            const char* hint = texts::T("match_setup.preset_editor.name_needed");
            const ImVec2 size = TextSize(fonts.caption, hint);
            DrawLabel(ImGui::GetWindowDrawList(), fonts.caption,
                      {right_start - Dp(14) - size.x, bar.y + (button - size.y) * 0.5f}, Col(color::warning), hint);
        }
        ImGui::SetCursorScreenPos({right_start, bar.y});
        bool leave = false;
        if (SecondaryButton("cancel_preset", texts::T("match_setup.buttons.cancel"), {end_button, button}, true,
                            Icon::Close))
            leave = true;
        ImGui::SameLine(0, Dp(8));
        if (PrimaryButton("save_preset", texts::T("match_setup.buttons.save"), {end_button, button}, blocked.empty(),
                          Icon::Check))
        {
            try
            {
                launcher::SaveMatchPreset(MatchPresetsFolder(), preset, MatchPresets().presets);
                Notice(texts::T("match_setup.rules.saved", {{"name", preset.name}}), NoticeKind::Success);
                leave = true;
            }
            catch (const std::exception& e) { Notice(e.what(), NoticeKind::Error); }
            presets_loaded_ = false;
        }
        EndCard();
        if (leave)
        {
            preset_edit_.reset();
            page_scroll_top_ = true;
        }
    }

    // A choice among names (a cheat, the skill) as a dropdown.
    static void CheatCombo(const char* id, int& cheat, const std::vector<const char*>& names)
    {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const char* shown = cheat >= 0 && std::size_t(cheat) < names.size() ? names[std::size_t(cheat)] : "?";
        if (ImGui::BeginCombo(id, shown))
        {
            for (std::size_t n = 0; n < names.size(); ++n)
            {
                // Its ID by its place: the names are in the player's language.
                const std::string label = std::string(names[n]) + "###" + std::to_string(n);
                if (ImGui::Selectable(label.c_str(), int(n) == cheat)) cheat = int(n);
            }
            ImGui::EndCombo();
        }
    }

    // The presets folder: match-presets beside the settings file.
    fs::path MatchPresetsFolder() const { return file_.path.parent_path() / "match-presets"; }

    // The presets, read again when the folder changes (a file added, removed
    // or replaced) or after the page changes one.
    const launcher::MatchPresetList& MatchPresets()
    {
        const auto folder = MatchPresetsFolder();
        std::error_code error;
        const auto changed = fs::last_write_time(folder, error);
        const auto time = error ? fs::file_time_type{} : changed;
        if (!presets_loaded_ || folder != presets_folder_ || time != presets_time_)
        {
            presets_folder_ = folder;
            presets_time_ = time;
            presets_ = launcher::LoadMatchPresets(folder);
            presets_loaded_ = true;
            preset_delete_ = -1;
        }
        return presets_;
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
        const fs::path folder = PathFromUtf8(chosen);
        switch (folder_purpose_)
        {
        case FolderPurpose::SaveFolder:
            // Relative to the settings file when inside its folder, as the
            // game and texture folders: the install can move with its saves.
            draft_.save_folder = SettingsPath(folder);
            dirty_ = true;
            return;
        case FolderPurpose::GameFolder: SelectGameFolder(folder); return;
        case FolderPurpose::TextureFolder:
            draft_.texture_folder = SettingsPath(folder);
            dirty_ = true;
            return;
        case FolderPurpose::DolphinMiis:
            if (const auto database = mscharged::platform::ResolveDolphinMiiDatabase(folder); !database.empty())
                AskImport(database, ImportKind::Miis);
            else
                Notice(texts::T("match_setup.import.no_mii_database", {{"file", "RFL_DB.dat"}, {"folder", chosen}}),
                       NoticeKind::Warning);
            return;
        case FolderPurpose::ProfileImport: ImportProfiles(folder); return;
        case FolderPurpose::DolphinSave:
            if (const auto save = mscharged::platform::ResolveDolphinSave(folder, Release().title_id, kSaveFile);
                !save.empty())
                AskImport(save, ImportKind::Save);
            else Notice(texts::T("match_setup.import.no_save", {{"folder", chosen}}), NoticeKind::Warning);
            return;
        }
    }

    void AskImport(const fs::path& source, ImportKind kind)
    {
        import_source_ = source;
        import_kind_ = kind;
        import_prompt_ = true;
    }

    void ImportDolphinSave()
    {
        try
        {
            const auto folder = SaveFolder();
            const auto result = mscharged::platform::ImportDolphinSave(import_source_, folder, SaveTitleOf(Release()),
                                                                       kSaveFile, folder / "backups");
            ForgetInstalledMiis();
            std::string files;
            for (const auto& name : result.files) files += (files.empty() ? "" : ", ") + name;
            Notice(result.backup.empty()
                       ? texts::T("match_setup.import.save_imported", {{"files", files}})
                       : texts::T("match_setup.import.save_imported_backup",
                                  {{"files", files}, {"path", PathUtf8(result.backup)}}),
                   NoticeKind::Success);
        }
        catch (const std::exception& e)
        {
            Notice(texts::T("match_setup.import.failed", {{"error", e.what()}}), NoticeKind::Error);
        }
    }

    void ImportDolphinMiis()
    {
        try
        {
            const auto folder = SaveFolder();
            const auto result = mscharged::platform::ImportDolphinMiis(import_source_, folder, SaveTitleOf(Release()),
                                                                       folder / "backups");
            ForgetInstalledMiis();
            std::string names;
            for (const auto& name : result.names) names += (names.empty() ? "" : ", ") + name;
            Notice(result.backup.empty()
                       ? texts::T("match_setup.import.miis_imported", {{"names", names}})
                       : texts::T("match_setup.import.miis_imported_backup",
                                  {{"names", names}, {"path", PathUtf8(result.backup)}}),
                   NoticeKind::Success);
        }
        catch (const std::exception& e)
        {
            Notice(texts::T("match_setup.import.failed", {{"error", e.what()}}), NoticeKind::Error);
        }
    }

    void DrawImportPrompt()
    {
        if (import_prompt_)
        {
            ImGui::OpenPopup("Import from Dolphin?");
            import_prompt_ = false;
        }
        const bool miis = import_kind_ == ImportKind::Miis;
        const auto& fonts = CurrentFonts();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({Dp(460), 0}, {Dp(640), FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(28, 24));
        if (ImGui::BeginPopupModal("Import from Dolphin?", nullptr, ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            TextColored(fonts.heading, color::text,
                        miis ? texts::T("match_setup.import.title_miis") : texts::T("match_setup.import.title_save"));
            TextWrappedColored(fonts.body, color::muted,
                               texts::T("match_setup.import.from", {{"path", PathUtf8(import_source_)}}).c_str());
            TextWrappedColored(fonts.body, color::muted,
                               texts::T("match_setup.import.into", {{"path", PathUtf8(SaveFolder())}}).c_str());
            TextWrappedColored(fonts.body, color::muted,
                               miis ? texts::T("match_setup.import.replace_miis")
                                    : texts::T("match_setup.import.replace_save"));
            ImGui::Dummy({0, Dp(8)});
            if (PrimaryButton("import_confirm", texts::T("match_setup.import.button"), Dp(140, 42), true, Icon::Copy))
            {
                ImGui::CloseCurrentPopup();
                if (miis) ImportDolphinMiis();
                else ImportDolphinSave();
            }
            ImGui::SameLine(0, Dp(10));
            if (SecondaryButton("import_cancel", texts::T("match_setup.buttons.cancel"), Dp(100, 42))
                || CancelPressed())
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
        return !library_scan_.Busy() && scanned_folder_ == draft_.folder && scanned_subfolders_ == draft_.folder_subfolders;
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
            row.checking = disc_check_.Busy();
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
            row.checking = row.chosen && disc_check_.Busy();
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // The game images: the game folder and one table of the images in it
    // (and the chosen one); clicking an image makes it the one Play starts.
    void GameImagesCard()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##images", texts::T("match_setup.images.title"), texts::T("match_setup.images.about"), Icon::Disc);
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
            const std::string text = !progress || progress->listing
                ? std::string(texts::T("match_setup.images.reading_folder"))
                : texts::T("match_setup.images.checking", {{"checked", std::to_string(progress->checked.load())},
                                                          {"found", std::to_string(progress->found.load())}});
            DrawLabel(ImGui::GetWindowDrawList(), fonts.body,
                      {at.x + Dp(44), at.y + (line - FontSize(fonts.body)) * 0.5f}, Col(color::muted), text.c_str());
            ImGui::Dummy({ImGui::GetContentRegionAvail().x, line});
        }
        else if (library_.empty())
            Callout(missing_folder ? texts::T("match_setup.images.folder_missing")
                                   : texts::T("match_setup.images.folder_empty"),
                    color::info, Icon::Info);

        // Footer: what the table leaves out, and a file from anywhere else.
        ImGui::Dummy({0, Dp(4)});
        std::string footer;
        if (library_other_games_ > 0) footer = texts::Count("match_setup.images.other_games", library_other_games_);
        if (library_limited_)
            footer += std::string(footer.empty() ? "" : " ") + texts::T("match_setup.images.limited");
        const char* choose = candidate_.Busy() ? texts::T("match_setup.images.checking_file")
                                                : texts::T("match_setup.images.choose_file");
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
                        config_ok_ && !options_.launch.disc && !DialogActive() && !candidate_.Busy(), Icon::Disc))
            Browse(ExistingFolder(GameFolder().path));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", texts::T("match_setup.images.choose_file_tooltip"));
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
        const char* change = texts::T("match_setup.images.change_folder");
        const char* subfolders = texts::T("match_setup.images.subfolders");
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
            status = texts::T("match_setup.images.reading");
            tint = color::info;
        }
        else if (unavailable)
        {
            status = texts::T("match_setup.images.not_found");
            tint = color::warning;
        }
        else
        {
            const auto count = std::count_if(library_.begin(), library_.end(), [](const LibraryImage& image) {
                return image.probe.kind == DiscProbe::Kind::Charged;
            });
            status = count == 0 ? std::string(texts::T("match_setup.images.no_images"))
                                : texts::Count("match_setup.images.count", count);
        }
        const float text_x = box.x + Dp(36 + 14), text_w = std::max(Dp(80), label_x - Dp(18) - text_x);
        const float lines = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
        const float top = center - lines * 0.5f;
        DrawPath(draw, fonts.label, {text_x, top}, text_w, color::text, PathUtf8(path));
        DrawClipped(draw, fonts.caption, {text_x, top + FontSize(fonts.label) + Dp(3)}, text_w, tint, status);
        if (picked) BrowseFolder(FolderPurpose::GameFolder, path);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", texts::T("match_setup.images.folder_tooltip", {{"path", PathUtf8(path)}}).c_str());

        DrawLabel(draw, fonts.caption, {label_x, center - FontSize(fonts.caption) * 0.5f}, Col(color::muted),
                  subfolders);
        ImGui::SetCursorScreenPos({toggle_x, center - Dp(14)});
        const auto before = GameFolder();
        if (Toggle("subfolders", &draft_.folder_subfolders)) GameFolderChanged(before);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", texts::Count("match_setup.images.subfolders_tooltip", kLibraryDepth).c_str());
        draw->AddRectFilled({divider_x, center - Dp(14)}, {divider_x + Dp(1), center + Dp(14)}, Col(color::border));
        ImGui::SetCursorScreenPos({refresh_x, center - Dp(17)});
        if (GhostButton("refresh_library", "", Dp(34, 34), config_ok_ && !library_scan_.Busy(), Icon::Refresh))
        {
            StartLibraryScan();
            if (!DiscSelection().empty() && !disc_check_.Busy()) CheckDisc();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("match_setup.images.refresh_tooltip"));
        ImGui::SetCursorScreenPos({change_x, center - Dp(17)});
        if (SecondaryButton("change_folder", change, {change_w, Dp(34)}, can_select))
            BrowseFolder(FolderPurpose::GameFolder, path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("match_setup.images.change_tooltip"));
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
            // The headers in the player's language, their IDs (after ###) the same in all.
            const auto header = [](const char* text, const char* id) { return std::string(text) + "###" + id; };
            ImGui::TableSetupColumn(header(texts::T("match_setup.images.columns.game"), "game").c_str(),
                                    ImGuiTableColumnFlags_WidthStretch, 0.0f, ColumnGame);
            ImGui::TableSetupColumn(header(texts::T("match_setup.images.columns.version"), "version").c_str(),
                                    fixed | ImGuiTableColumnFlags_DefaultSort, Dp(narrow ? 100 : 112), ColumnVersion);
            if (!narrow)
                ImGui::TableSetupColumn(header(texts::T("match_setup.images.columns.file"), "file").c_str(),
                                        fixed | ImGuiTableColumnFlags_PreferSortDescending, Dp(66), ColumnFile);
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
            note = texts::T("match_setup.images.note.command_line");
            note_tint = color::info;
        }
        else if (row.checking)
        {
            note = texts::T("match_setup.images.note.checking");
            note_tint = color::info;
        }
        else if (row.missing)
        {
            note = check_.folder_missing ? texts::T("match_setup.images.note.folder_gone")
                                         : texts::T("match_setup.images.note.file_gone");
            note_tint = color::warning;
        }
        else if (state == ImageState::Unsupported)
        {
            const auto* release = FindRelease(info);
            bool known_id = false;
            for (std::size_t n = 0; n < ReleaseCount(); ++n) known_id = known_id || info.game_id == ReleaseAt(n).game_id;
            note = release    ? texts::T("match_setup.images.note.build_unsupported")
                 : known_id ? texts::T("match_setup.images.note.revision_unsupported")
                            : texts::T("match_setup.images.note.unsupported");
            reason = UnplayableReason(info, release) + "\n"
                + texts::T("match_setup.images.build_runs", {{"releases", PlayableReleasesText()}});
            note_tint = color::warning;
        }
        else if (state == ImageState::Unreadable && !image.probe.error.empty())
        {
            note = texts::T("match_setup.images.note.unreadable");
            reason = image.probe.error;
            note_tint = color::danger;
        }
        else if (const auto* release = charged ? FindRelease(info) : nullptr; release && !ReleaseNote(*release).empty())
            note = ReleaseNote(*release);
        if (row.outside && note.empty()) note = texts::T("match_setup.images.note.outside");

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
            if (row.chosen && !row.missing) tip += std::string("\n") + texts::T("match_setup.images.tip.chosen");
            else if (can_choose) tip += std::string("\n") + texts::T("match_setup.images.tip.choose");
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
                        texts::T("match_setup.images.revision", {{"revision", std::to_string(unsigned(info.revision))}})
                            + "  \xC2\xB7  " + info.game_id);
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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("match_setup.images.open_folder_tooltip"));
        }
        ImGui::PopID();
    }

    // Makes this image the one Play starts and says so.
    void UseImage(const fs::path& path, const DiscInfo& info, std::uintmax_t size)
    {
        // An image dropped or picked earlier and still being checked doesn't replace this choice.
        candidate_.Drop();
        Pin(path, info, size);
        Notice(texts::T("match_setup.images.play_now",
                        {{"file", PathUtf8(path.filename())}, {"version", RegionRevision(info)}}),
               NoticeKind::Success);
    }

    void LanguageControl()
    {
        const auto& release = Release();
        std::vector<const char*> values{"auto"}, labels{texts::T("match_setup.language.auto")};
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
            TextWrappedColored(
                CurrentFonts().caption, color::warning,
                texts::T("match_setup.language.unavailable", {{"language", choice.language->label}}).c_str());
        else if (choice.automatic)
            TextWrappedColored(
                CurrentFonts().caption, color::muted,
                texts::T("match_setup.language.automatic", {{"language", choice.language->label}}).c_str());
    }

    // Whether the game's picture fills its window or display: stretched, or
    // with the wider view.
    bool PictureFills() const
    {
        return draft_.aspect == "stretch" || (draft_.aspect == "16:9" && draft_.wider_view);
    }

    void AntialiasingKindChoice()
    {
        const mscharged::AntialiasingChoice* selected = nullptr;
        for (const auto& choice : kAntialiasingChoices)
            if (draft_.antialiasing == choice.value) selected = &choice;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##antialiasing", selected ? AntialiasingName(*selected) : draft_.antialiasing.c_str()))
        {
            for (const auto& choice : kAntialiasingChoices)
                if (ImGui::Selectable(StableLabel(AntialiasingName(choice), choice.value).c_str(), selected == &choice))
                { draft_.antialiasing = choice.value; dirty_ = true; }
            ImGui::EndCombo();
        }
        // The chosen kind's note; the launcher reads only known values.
        for (const auto& choice : kAntialiasingChoices)
            if (draft_.antialiasing == choice.value)
                TextWrappedColored(CurrentFonts().caption, color::muted, AntialiasingAbout(choice));
    }

    // An antialiasing kind's name and note (kAntialiasingChoices) in the
    // player's language.
    static const char* AntialiasingName(const mscharged::AntialiasingChoice& choice)
    {
        const std::string_view value = choice.value;
        if (value == "off") return texts::T("settings.display.antialiasing.off.name");
        if (value == "fxaa") return texts::T("settings.display.antialiasing.fxaa.name");
        if (value == "4x") return texts::T("settings.display.antialiasing.msaa.name");
        if (value == "4x_fxaa") return texts::T("settings.display.antialiasing.msaa_fxaa.name");
        if (value == "4x_ssaa") return texts::T("settings.display.antialiasing.ssaa.name");
        return choice.label;
    }
    static const char* AntialiasingAbout(const mscharged::AntialiasingChoice& choice)
    {
        const std::string_view value = choice.value;
        if (value == "off") return texts::T("settings.display.antialiasing.off.about");
        if (value == "fxaa") return texts::T("settings.display.antialiasing.fxaa.about");
        if (value == "4x") return texts::T("settings.display.antialiasing.msaa.about");
        if (value == "4x_fxaa") return texts::T("settings.display.antialiasing.msaa_fxaa.about");
        if (value == "4x_ssaa") return texts::T("settings.display.antialiasing.ssaa.about");
        return choice.description;
    }

    // A label in the player's language for a widget that takes its ImGui ID
    // from its label (Selectable, table header): the ID stays `id` in every
    // language.
    static std::string StableLabel(const char* text, std::string_view id)
    {
        return std::string(text).append("###").append(id);
    }


    // Gamepad `slot`'s profile as a two-column table: pad control, then Wii input.
    void GamepadLayoutTooltip(std::size_t slot)
    {
        const auto& profile = draft_.PadProfileOf(slot);
        const auto type = PadType(int(slot));
        const bool builtin = profile.follows.has_value();
        const auto family = platform::GamepadFamilyOf(type);
        const auto bindings = builtin ? platform::DefaultGamepadBindings(family, *profile.follows)
                                      : platform::ParseGamepadBindings(profile.inputs);
        const auto& fonts = CurrentFonts();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(16, 14));
        if (ImGui::BeginTooltip())
        {
            TextColored(fonts.label, color::text,
                        (builtin ? texts::T("settings.controls.pad_tooltip.profile_family",
                                            {{"name", profile.name}, {"family", platform::GamepadFamilyName(family)}})
                                 : texts::T("settings.controls.pad_tooltip.profile", {{"name", profile.name}}))
                            .c_str());
            TextColored(fonts.caption, color::muted, texts::T("settings.controls.pad_tooltip.about"));
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
            table(texts::T("settings.controls.pad_tooltip.buttons"), "##pad_buttons", [&] {
                for (std::size_t n = 0; n < GamepadActionC; ++n)
                    row(PadBindingLabel(bindings[n], type), PadActionName(n));
            });
            table(texts::T("settings.controls.pad_tooltip.nunchuk_motion"), "##pad_sticks", [&] {
                row(NunchukStickName(profile), texts::T("settings.controls.pad.nunchuk_stick"));
                for (std::size_t n = GamepadActionC; n < GamepadActionRecenterPointer; ++n)
                    row(PadBindingLabel(bindings[n], type), PadActionName(n));
                row(PointerName(profile), texts::T("settings.controls.pad.pointer"));
                if (profile.gyro_pointer)
                    row(PadBindingLabel(bindings[GamepadActionRecenterPointer], type),
                        PadActionName(GamepadActionRecenterPointer));
            });
            ImGui::Dummy({0, Dp(6)});
            const std::string inner = std::to_string(profile.inner_deadzone),
                              outer = std::to_string(profile.outer_deadzone),
                              trigger = std::to_string(profile.trigger_press);
            TextColored(fonts.caption, color::muted,
                        (profile.gyro_pointer
                             ? texts::T("settings.controls.pad_tooltip.feel_gyro",
                                        {{"inner", inner}, {"outer", outer}, {"trigger", trigger},
                                         {"gyro", std::to_string(profile.gyro_speed)}})
                             : texts::T("settings.controls.pad_tooltip.feel",
                                        {{"inner", inner}, {"outer", outer}, {"trigger", trigger}}))
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
            const int direction = input - platform::kGamepadStickBase;
            const std::string stick = direction >= platform::RightStickUp
                ? pick(texts::T("settings.controls.pad.right_stick"), texts::T("settings.controls.pad.right_stick"),
                       texts::T("settings.controls.pad.right_stick"), texts::T("settings.controls.pad.c_stick"))
                : std::string(texts::T("settings.controls.pad.left_stick"));
            switch (direction % 4)
            {
            case 0: return texts::T("settings.controls.pad.stick_up", {{"stick", stick}});
            case 1: return texts::T("settings.controls.pad.stick_down", {{"stick", stick}});
            case 2: return texts::T("settings.controls.pad.stick_left", {{"stick", stick}});
            default: return texts::T("settings.controls.pad.stick_right", {{"stick", stick}});
            }
        }
        if (input < 0 || input >= SDL_GAMEPAD_BUTTON_COUNT) return texts::T("settings.controls.pad.not_set");
        const auto button = SDL_GamepadButton(input);
        if (button <= SDL_GAMEPAD_BUTTON_NORTH)
        {
            switch (SDL_GetGamepadButtonLabelForType(type, button))
            {
            case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
            case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
            case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
            case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return texts::T("settings.controls.pad.cross");
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return texts::T("settings.controls.pad.circle");
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return texts::T("settings.controls.pad.square");
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return texts::T("settings.controls.pad.triangle");
            default: break;
            }
            static const char* const face[] = {"A", "B", "X", "Y"};
            return face[button];
        }
        switch (button)
        {
        case SDL_GAMEPAD_BUTTON_BACK:
            return pick(texts::T("settings.controls.pad.back"), texts::T("settings.controls.pad.share"),
                        texts::T("settings.controls.pad.minus"));
        case SDL_GAMEPAD_BUTTON_GUIDE:
            return pick(texts::T("settings.controls.pad.guide"), "PS", texts::T("settings.controls.pad.home"));
        case SDL_GAMEPAD_BUTTON_START:
            return pick(texts::T("settings.controls.pad.start"), texts::T("settings.controls.pad.options"),
                        texts::T("settings.controls.pad.plus"), texts::T("settings.controls.pad.start"));
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            return pick(texts::T("settings.controls.pad.left_stick_click"), "L3",
                        texts::T("settings.controls.pad.left_stick_click"));
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            return pick(texts::T("settings.controls.pad.right_stick_click"), "R3",
                        texts::T("settings.controls.pad.right_stick_click"));
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return pick("LB", "L1", "L");
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return pick("RB", "R1", "R", "Z");
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return texts::T("settings.controls.pad.dpad_up");
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return texts::T("settings.controls.pad.dpad_down");
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return texts::T("settings.controls.pad.dpad_left");
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return texts::T("settings.controls.pad.dpad_right");
        case SDL_GAMEPAD_BUTTON_MISC1:
            return pick(texts::T("settings.controls.pad.share"), texts::T("settings.controls.pad.mic"),
                        texts::T("settings.controls.pad.capture"));
        case SDL_GAMEPAD_BUTTON_MISC3:
            return gamecube ? texts::T("settings.controls.pad.r_click") : texts::T("settings.controls.pad.misc3");
        case SDL_GAMEPAD_BUTTON_MISC4:
            return gamecube ? texts::T("settings.controls.pad.l_click") : texts::T("settings.controls.pad.misc4");
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return texts::T("settings.controls.pad.paddle", {{"paddle", "R1"}});
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return texts::T("settings.controls.pad.paddle", {{"paddle", "L1"}});
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return texts::T("settings.controls.pad.paddle", {{"paddle", "R2"}});
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return texts::T("settings.controls.pad.paddle", {{"paddle", "L2"}});
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return texts::T("settings.controls.pad.touchpad");
        default: return texts::T("settings.controls.pad.button", {{"number", std::to_string(int(button))}});
        }
    }

    // The stick of the Nunchuk stick and what moves the pointer.
    static const char* NunchukStickName(const Settings::PadProfile& profile)
    {
        return profile.sticks == GamepadSticks::Swapped ? texts::T("settings.controls.pad.right_stick")
                                                        : texts::T("settings.controls.pad.left_stick");
    }
    static const char* PointerName(const Settings::PadProfile& profile)
    {
        return profile.gyro_pointer                    ? texts::T("settings.controls.pad.gyro")
            : profile.sticks == GamepadSticks::Split ? texts::T("settings.controls.pad.right_stick")
                                                     : texts::T("settings.controls.pad.left_stick");
    }

    // A gamepad action's name and note (kGamepadActions) in the player's
    // language: the Wii input's (common.json's pad_actions), and the gyro
    // pointer's recentring.
    static const char* PadActionName(std::size_t action)
    {
        if (action == GamepadActionRecenterPointer) return texts::T("settings.controls.pad.recenter_pointer.name");
        return texts::TableText("pad_actions", kGamepadActions[action].key, "name");
    }
    static const char* PadActionAbout(std::size_t action)
    {
        if (!kGamepadActions[action].note) return nullptr;
        if (action == GamepadActionRecenterPointer) return texts::T("settings.controls.pad.recenter_pointer.about");
        return texts::TableText("pad_actions", kGamepadActions[action].key, "about");
    }

    static std::string PadBindingLabel(const platform::GamepadBinding& binding, SDL_GamepadType type)
    {
        std::string text = PadInputLabel(binding[0], type);
        for (std::size_t n = 1; n < platform::GamepadBindingSize(binding); ++n)
            text += " / " + PadInputLabel(binding[n], type);
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
        const std::string preview = listed ? label(draft_.width, draft_.height)
            : texts::T("settings.display.window_size.current", {{"size", label(draft_.width, draft_.height)}});
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##size", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (!listed) ImGui::Selectable(StableLabel(preview.c_str(), "current").c_str(), true);
            for (const auto& size : sizes)
                if (ImGui::Selectable(label(size[0], size[1]).c_str(), size == current))
                { draft_.width = size[0]; draft_.height = size[1]; dirty_ = true; }
            ImGui::EndCombo();
        }
        if (!fits(draft_.width, draft_.height))
        {
            const auto shrunk = platform::FitWindowSize(draft_.width, draft_.height, usable.w, usable.h);
            const std::string note = texts::T("settings.display.window_size.too_large",
                                              {{"width", std::to_string(shrunk.width)},
                                               {"height", std::to_string(shrunk.height)}});
            TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
        }
        else if (!any_shape)
        {
            // The game gives the window the picture's shape: the same width.
            const auto shaped = SnapToShape(draft_.width, draft_.height, shape_w, shape_h);
            if (shaped.height != draft_.height)
            {
                const std::string note = texts::T("settings.display.window_size.not_shape",
                                                  {{"shape", four_three ? "4:3" : "16:9"},
                                                   {"width", std::to_string(shaped.width)},
                                                   {"height", std::to_string(shaped.height)}});
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
        const std::string desktop_label = desktop
            ? texts::T("settings.display.fullscreen_resolution.desktop_size",
                       {{"size", SizeLabel(desktop->w, desktop->h, false)}})
            : std::string(texts::T("settings.display.fullscreen_resolution.desktop"));
        const auto chosen = ParseResolution(draft_.fullscreen_resolution);
        const std::array<int, 2> current{chosen.width, chosen.height};
        const bool listed = chosen.width == 0 || std::find(sizes.begin(), sizes.end(), current) != sizes.end();
        const std::string preview = chosen.width == 0 ? desktop_label
            : listed ? SizeLabel(chosen.width, chosen.height, true)
                     : texts::T("settings.display.fullscreen_resolution.not_on_display",
                                {{"size", SizeLabel(chosen.width, chosen.height, true)}});
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##fullscreen_resolution", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable(StableLabel(desktop_label.c_str(), "desktop").c_str(), chosen.width == 0))
            { draft_.fullscreen_resolution = std::string(kDesktopResolution); dirty_ = true; }
            if (!listed) ImGui::Selectable(StableLabel(preview.c_str(), "current").c_str(), true);
            for (const auto& size : sizes)
                if (ImGui::Selectable(SizeLabel(size[0], size[1], true).c_str(), size == current))
                { draft_.fullscreen_resolution = FormatResolution(size[0], size[1]); dirty_ = true; }
            ImGui::EndCombo();
        }
        if (!listed)
            TextWrappedColored(CurrentFonts().caption, color::warning,
                               texts::T("settings.display.fullscreen_resolution.no_such_mode"));
        else if (chosen.width && desktop && desktop->w > 0 && desktop->h > 0
                 && std::abs(double(chosen.width) * desktop->h / (double(chosen.height) * desktop->w) - 1.0) > 0.01)
        {
            const std::string note = texts::T("settings.display.fullscreen_resolution.other_shape",
                                              {{"size", SizeLabel(desktop->w, desktop->h, false)}});
            TextWrappedColored(CurrentFonts().caption, color::muted, note.c_str());
        }
    }

    void DisplayChoice()
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const char* default_display = texts::T("settings.display.screen.display.default");
        std::string preview = default_display;
        if (draft_.monitor > 0)
            preview = draft_.monitor <= int(displays_.size())
                ? displays_[size_t(draft_.monitor - 1)]
                : texts::T("settings.display.screen.display.not_connected",
                           {{"number", std::to_string(draft_.monitor)}});
        ImGui::SetNextItemWidth(width);
        ImGui::BeginDisabled(displays_.size() <= 1 && draft_.monitor == 0);
        if (ImGui::BeginCombo("##monitor", preview.c_str()))
        {
            if (ImGui::Selectable(StableLabel(default_display, "default").c_str(), draft_.monitor == 0))
            { draft_.monitor = 0; dirty_ = true; }
            for (size_t i = 0; i < displays_.size(); ++i)
                if (ImGui::Selectable(displays_[i].c_str(), draft_.monitor == int(i) + 1))
                { draft_.monitor = int(i) + 1; dirty_ = true; }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
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
        {
            wii_next_probe_ns_ = now + 2000000000ull;
            wii_probe_.Start(250);
            // HID unavailable (the probe did not start): no remotes, and the
            // Players card stops looking instead of trying every frame.
            if (!wii_probe_.Active())
            {
                AcceptWiimoteProbe({});
                wii_known_ = true;
            }
        }
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
        const std::string model = remote && remote->plus ? texts::T("settings.controls.remote.wii_remote_plus")
                                                         : texts::T("settings.controls.remote.wii_remote");
        const int slot = platform::WiimoteDolphinBarSlot(id);
        if (slot >= 0)
            return texts::T("settings.controls.remote.in_dolphinbar",
                            {{"model", model}, {"slot", std::to_string(slot + 1)}});
        const auto address = platform::WiimoteBluetoothAddress(id);
        return texts::T("settings.controls.remote.over_bluetooth",
                        {{"model", model},
                         {"address", address.size() > 8 ? address.substr(address.size() - 8) : address}});
    }

    // Windows: connect and pair Wii Remotes over Bluetooth (wiimote_bluetooth.h).
    void BluetoothRows()
    {
        const auto search = bluetooth_search_.Status();
        if (!search.supported || !BeginRows("##bluetooth_rows")) return;
        Row(texts::T("settings.controls.bluetooth.scanning.name"),
            !search.adapter         ? texts::T("settings.controls.bluetooth.scanning.no_adapter")
            : draft_.bluetooth_search ? texts::T("settings.controls.bluetooth.scanning.on")
                                      : texts::T("settings.controls.bluetooth.scanning.off"));
        if (Toggle("##bluetooth_search", &draft_.bluetooth_search)) dirty_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.controls.bluetooth.scanning.tooltip"));
        std::string text = search.syncing     ? texts::T("settings.controls.bluetooth.pairing.syncing")
                           : search.resetting ? texts::T("settings.controls.bluetooth.pairing.forgetting")
                                              : texts::T("settings.controls.bluetooth.pairing.about");
        if (!search.syncing && !search.resetting && !search.last.empty())
            text += " " + texts::T("settings.controls.bluetooth.pairing.last", {{"result", search.last}});
        Row(texts::T("settings.controls.bluetooth.pairing.name"), text.c_str());
        const bool idle = !search.syncing && !search.resetting;
        const float gap = Dp(8), width = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3;
        if (SecondaryButton("##bluetooth_refresh", texts::T("settings.controls.bluetooth.refresh"), {width, Dp(36)},
                            idle))
            bluetooth_search_.Refresh();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.controls.bluetooth.refresh_tooltip"));
        ImGui::SameLine(0, gap);
        if (SecondaryButton("##bluetooth_sync",
                            search.syncing ? texts::T("settings.controls.bluetooth.syncing")
                                           : texts::T("settings.controls.bluetooth.sync"),
                            {width, Dp(36)}, idle))
            bluetooth_search_.Sync();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.controls.bluetooth.sync_tooltip"));
        ImGui::SameLine(0, gap);
        if (SecondaryButton("##bluetooth_reset", texts::T("settings.controls.bluetooth.reset"), {width, Dp(36)}, idle))
            bluetooth_search_.Reset();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.controls.bluetooth.reset_tooltip"));
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
        BeginCard("##players", texts::T("settings.controls.players.title"), texts::T("settings.controls.players.about"),
                  Icon::Gamepad);
        bool chip = false;
        const auto next_chip = [&] { if (chip) ImGui::SameLine(0, Dp(8)); chip = true; };
        if (bar)
        {
            next_chip();
            Chip("DolphinBar", color::success, Icon::Check);
        }
        else if (status.dolphinbar_other_mode)
        {
            next_chip();
            Chip(texts::T("settings.controls.players.dolphinbar_mode"), color::warning, Icon::Warning);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", texts::T("settings.controls.players.dolphinbar_mode_tooltip"));
        }
        if (bluetooth)
        {
            next_chip();
            const auto text = texts::Count("settings.controls.players.bluetooth_remotes", bluetooth);
            Chip(text.c_str(), color::success, Icon::Check);
        }
        if (status.no_permission)
        {
            next_chip();
            Chip(texts::T("settings.controls.players.no_access"), color::warning, Icon::Warning);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", texts::T("settings.controls.players.no_access_tooltip"));
        }
        if (!chip)
        {
            Chip(wii_known_ ? texts::T("settings.controls.players.no_remote")
                            : texts::T("settings.controls.players.looking"),
                 color::muted, wii_known_ ? Icon::Info : Icon::None);
            if (wii_known_)
            {
                ImGui::SameLine(0, Dp(10));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(5));
                TextColored(CurrentFonts().caption, color::muted, texts::T("settings.controls.players.no_remote_hint"));
            }
        }

        const auto name = [&](const std::string& device) {
            if (device == "keyboard") return std::string(texts::T("settings.controls.players.keyboard"));
            if (device == "off") return std::string(texts::T("settings.controls.players.off"));
            if (device.rfind("gamepad", 0) == 0)
            {
                const int slot = device.back() - '1';
                const std::string number = std::to_string(slot + 1);
                return slot < int(gamepad_names_.size())
                    ? texts::T("settings.controls.players.gamepad_named",
                               {{"number", number}, {"name", gamepad_names_[slot]}})
                    : texts::T("settings.controls.players.gamepad_missing", {{"number", number}});
            }
            const auto* remote = ConnectedRemote(device);
            if (!remote) return texts::T("settings.controls.players.remote_missing", {{"remote", RemoteName(device)}});
            if (remote->nunchuk)
                return texts::T("settings.controls.players.remote_nunchuk", {{"remote", RemoteName(device)}});
            return RemoteName(device);
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
                            texts::T("settings.controls.players.player", {{"number", std::to_string(n + 1)}}).c_str());
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
                        const auto label = other >= 0
                            ? texts::T("settings.controls.players.swap",
                                       {{"device", name(device)}, {"player", std::to_string(other + 1)}})
                            : name(device);
                        if (ImGui::Selectable(StableLabel(label.c_str(), device).c_str(), players[n] == device))
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
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", texts::T("settings.controls.players.missing_tooltip"));
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
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", texts::T("settings.controls.players.keys_tooltip"));
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
            const std::string rule = texts::T("settings.controls.players.drop_out_rule");
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
        BluetoothRows();
        if (status.no_permission && BeginRows("##wiimote_access_rows"))
        {
#if defined(__linux__)
            Row(texts::T("settings.controls.players.access.name"), texts::T("settings.controls.players.access.udev"));
            if (SecondaryButton("##copy_udev", texts::T("settings.controls.players.access.copy_commands"),
                                {ImGui::GetContentRegionAvail().x, Dp(36)}, true, Icon::Copy))
            {
                if (SDL_SetClipboardText(kWiimoteUdevCommands))
                    Notice(texts::T("settings.controls.players.access.copied"), NoticeKind::Success);
            }
#else
            Row(texts::T("settings.controls.players.access.name"), texts::T("settings.controls.players.access.in_use"));
            ImGui::Dummy({0, 0});
#endif
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

    // The keyboard and gamepad views save their changes when going back,
    // unless other changes waited to be saved when they opened: those are not
    // saved without asking, the bottom bar keeps them all.
    void OpenKeyboardView()
    {
        keyboard_view_ = true;
        view_opened_clean_ = !dirty_;
        input_capture_.Cancel();
    }

    void CloseKeyboardView()
    {
        keyboard_view_ = false;
        input_capture_.Cancel();
        if (view_opened_clean_ && dirty_ && config_ok_) Save();
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
        if (!input_capture_.Waits(InputCapture::Kind::Key)) return;
        const int action = input_capture_.action, slot = input_capture_.slot;
        input_capture_.Cancel();
        const std::string name = platform::KeyInputName(code);
        if (name.empty()) return;
        if (const auto hotkey = DraftHotkeys().Find(code); hotkey != HotkeyCount)
        {
            Notice(texts::T("settings.controls.keyboard.is_hotkey",
                            {{"key", name}, {"hotkey", texts::HotkeyActionName(kHotkeys[hotkey])}}),
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
                    Notice(texts::T("settings.controls.keyboard.only_key",
                                    {{"key", name}, {"action", texts::KeyActionName(kKeyActions[other])}}),
                           NoticeKind::Warning);
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
        Notice(moved >= 0 ? texts::T("settings.controls.keyboard.now_presses_moved",
                                     {{"key", name}, {"action", texts::KeyActionName(kKeyActions[action])},
                                      {"from", texts::KeyActionName(kKeyActions[moved])}})
                          : texts::T("settings.controls.keyboard.now_presses",
                                     {{"key", name}, {"action", texts::KeyActionName(kKeyActions[action])}}),
               NoticeKind::Info);
    }

    // A key pressed while a hotkey's keycaps wait (input_capture_: its
    // HotkeyIndex), with the Ctrl, Shift and Alt held (`mod`): Escape stops
    // waiting, Backspace or Delete leaves the hotkey unused, a modifier waits
    // for the key it goes with. The same keys as another hotkey, the Windows
    // or Command key, or a keyboard & mouse key pressed alone are refused.
    void CaptureHotkey(SDL_Scancode code, SDL_Keymod mod)
    {
        const int which = input_capture_.Waits(InputCapture::Kind::Hotkey) ? input_capture_.action : -1;
        if (which < 0 || which >= int(HotkeyCount)) return;
        if (code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI) return;
        input_capture_.Cancel();
        const char* label = texts::HotkeyActionName(kHotkeys[std::size_t(which)]);
        const platform::Hotkey hotkey{code, platform::HotkeyModifiers(mod)};
        if (hotkey.modifiers == SDL_KMOD_NONE && !(mod & SDL_KMOD_GUI))
        {
            if (code == SDL_SCANCODE_ESCAPE) return;
            if (code == SDL_SCANCODE_BACKSPACE || code == SDL_SCANCODE_DELETE)
            {
                if (draft_.hotkeys[std::size_t(which)] == kNoHotkey) return;
                draft_.hotkeys[std::size_t(which)] = kNoHotkey;
                dirty_ = true;
                Notice(texts::T("settings.controls.hotkeys.no_hotkey_now", {{"hotkey", label}}), NoticeKind::Info);
                return;
            }
        }
        if (mod & SDL_KMOD_GUI)
        {
            Notice(texts::T("settings.controls.hotkeys.system_keys"), NoticeKind::Warning);
            return;
        }
        const std::string name = platform::HotkeyLabel(hotkey);
        if (!platform::HotkeyAllowed(hotkey) || name.empty()) return;
        const auto hotkeys = DraftHotkeys();
        for (std::size_t other = 0; other < HotkeyCount; ++other)
            if (hotkeys.keys[other] == hotkey)
            {
                if (int(other) != which)
                    Notice(texts::T("settings.controls.hotkeys.already",
                                    {{"key", name}, {"hotkey", texts::HotkeyActionName(kHotkeys[other])}}),
                           NoticeKind::Warning);
                return;
            }
        // A key pressed alone would also press its keyboard & mouse action.
        if (hotkey.modifiers == SDL_KMOD_NONE)
        {
            const auto bindings = DraftBindings();
            for (std::size_t action = 0; action < KeyActionCount; ++action)
                if (bindings[action][0] == code || bindings[action][1] == code)
                {
                    Notice(texts::T("settings.controls.hotkeys.key_taken",
                                    {{"key", name}, {"action", texts::KeyActionName(kKeyActions[action])}}),
                           NoticeKind::Warning);
                    return;
                }
        }
        draft_.hotkeys[std::size_t(which)] = platform::HotkeyName(hotkey);
        dirty_ = true;
        Notice(texts::T("settings.controls.hotkeys.now_hotkey", {{"key", name}, {"hotkey", label}}), NoticeKind::Info);
    }

    // [hotkeys]: keys of the game window, whoever plays, each a key alone or
    // with Ctrl, Shift or Alt. Whether matches start with the HUD is the
    // game's CONTROLS setting.
    void HotkeysCard()
    {
        BeginCard("##hotkeys", texts::T("settings.controls.hotkeys.title"), texts::T("settings.controls.hotkeys.about"),
                  Icon::Keyboard);
        const auto hotkeys = DraftHotkeys();
        bool clicked = false;
        const auto hotkey = [&](HotkeyIndex which) {
            Row(texts::HotkeyActionName(kHotkeys[which]), texts::HotkeyActionAbout(kHotkeys[which]));
            const bool waiting = input_capture_.WaitsFor(InputCapture::Kind::Hotkey, int(which));
            std::vector<std::string> keys;
            if (waiting)
            {
                // The modifiers held so far, then the key still to come.
                const SDL_Keymod held = platform::HotkeyModifiers(SDL_GetModState());
                keys = platform::HotkeyKeys({SDL_SCANCODE_A, held});
                keys.back() = keys.size() > 1 ? "..." : texts::T("settings.controls.hotkeys.press_keys");
            }
            else if (hotkeys[which].key) keys = platform::HotkeyKeys(hotkeys[which]);
            else keys = {texts::T("settings.controls.hotkeys.not_set")};
            ImGui::PushID(int(which));
            if (KeyComboButton("##hotkey", keys, waiting))
            {
                clicked = true;
                if (waiting) input_capture_.Cancel();
                else input_capture_.Wait(InputCapture::Kind::Hotkey, int(which));
            }
            if (ImGui::IsItemHovered() && !waiting)
                ImGui::SetTooltip("%s", texts::T("settings.controls.hotkeys.tooltip"));
            ImGui::PopID();
        };
        if (BeginRows("##hotkey_rows"))
        {
            hotkey(HotkeyScreenshot);
            hotkey(HotkeyFullscreen);
            hotkey(HotkeyStretch);
            hotkey(HotkeyResolutionUp);
            hotkey(HotkeyResolutionDown);
            hotkey(HotkeyPanel);
            hotkey(HotkeyOptions);
            hotkey(HotkeyHud);
            EndRows();
        }
        if (SecondaryButton("##reset_hotkeys", texts::T("settings.controls.hotkeys.reset"), Dp(140, 36),
                            draft_.hotkeys != DefaultHotkeyNames(), Icon::Refresh))
        {
            draft_.hotkeys = DefaultHotkeyNames();
            input_capture_.Cancel();
            dirty_ = true;
        }
        EndCard();
        // A click anywhere else stops waiting for keys.
        if (input_capture_.Waits(InputCapture::Kind::Hotkey) && !clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) input_capture_.Cancel();
    }

    // What left and right click do: the actions that take them, or A and B
    // while no action does.
    static std::string PointerNote(const platform::KeyBindings& bindings)
    {
        const auto click = [&](const char* id, Uint8 button, const char* fallback) {
            std::string actions;
            const auto input = platform::MouseKeyInput(button);
            for (std::size_t n = 0; n < KeyActionCount; ++n)
                if (bindings[n][0] == input || bindings[n][1] == input)
                    actions += (actions.empty() ? "" : ", ") + std::string(texts::KeyActionName(kKeyActions[n]));
            if (actions.empty())
                actions = texts::T("settings.controls.keyboard.mouse.until_taken", {{"button", fallback}});
            return texts::T(id, {{"what", actions}});
        };
        return click("settings.controls.keyboard.mouse.left_click", SDL_BUTTON_LEFT, "A") + "; "
               + click("settings.controls.keyboard.mouse.right_click", SDL_BUTTON_RIGHT, "B");
    }

    void KeyboardView()
    {
        if (SecondaryButton("##back", texts::T("settings.controls.back_to_players"), Dp(124, 36), true, Icon::Back))
        { CloseKeyboardView(); return; }
        ImGui::Dummy(Dp(0, 6));
        PageHeader(texts::T("settings.controls.keyboard.title"), texts::T("settings.controls.keyboard.about"));
        const auto bindings = DraftBindings();
        bool clicked = false, on_waiting = false;
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
                    TextColored(CurrentFonts().label, color::text, texts::KeyActionName(kKeyActions[n]));
                    if (const char* note = texts::KeyActionAbout(kKeyActions[n]))
                    {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
                        TextColored(CurrentFonts().caption, color::muted, note);
                    }
                    ImGui::TableSetColumnIndex(1);
                    for (int slot = 0; slot < 2; ++slot)
                    {
                        const platform::KeyInput code = bindings[n][slot];
                        if (slot == 1 && code == platform::kKeyNone && bindings[n][0] == platform::kKeyNone) break;
                        const bool waiting = input_capture_.WaitsFor(InputCapture::Kind::Key, int(n), slot);
                        const std::string label = waiting ? texts::T("settings.controls.keyboard.press_key")
                            : code != platform::kKeyNone ? platform::KeyInputName(code) : "+";
                        if (slot) ImGui::SameLine(0, Dp(6));
                        ImGui::PushID(slot);
                        if (KeyCapButton("##key", label.c_str(), waiting))
                        {
                            clicked = true;
                            // A click on the waiting keycap is the left button.
                            if (waiting) CaptureKey(platform::MouseKeyInput(SDL_BUTTON_LEFT));
                            else input_capture_.Wait(InputCapture::Kind::Key, int(n), slot);
                        }
                        if (waiting && ImGui::IsItemHovered()) on_waiting = true;
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", waiting ? texts::T("settings.controls.keyboard.waiting_tooltip")
                                                    : code != platform::kKeyNone
                                                        ? texts::T("settings.controls.keyboard.change_tooltip")
                                                        : texts::T("settings.controls.keyboard.add_tooltip"));
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
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", texts::T("settings.controls.keyboard.remove_tooltip"));
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        };
        BeginCard("##remote_keys", texts::T("settings.controls.keyboard.remote.title"),
                  texts::T("settings.controls.keyboard.remote.about"), Icon::Keyboard);
        rows(KeyActionA, KeyActionStickUp);
        EndCard();
        BeginCard("##nunchuk_keys", texts::T("settings.controls.keyboard.nunchuk.title"),
                  texts::T("settings.controls.keyboard.nunchuk.about"), Icon::Gamepad);
        rows(KeyActionStickUp, KeyActionCount);
        EndCard();
        BeginCard("##mouse_keys", texts::T("settings.controls.keyboard.mouse.title"),
                  texts::T("settings.controls.keyboard.mouse.about"), Icon::Mouse);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
        if (ImGui::BeginTable("##fixed_keys", 2))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch);
            KeyRow(texts::T("settings.controls.pad.pointer"), {texts::T("settings.controls.keyboard.mouse.mouse")},
                   PointerNote(bindings).c_str(), texts::T("settings.controls.keyboard.mouse.point"));
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        if (SecondaryButton("##reset_keys", texts::T("settings.controls.keyboard.reset"), Dp(180, 36),
                            draft_.keys != Settings::DefaultKeys(), Icon::Refresh))
        {
            draft_.keys = Settings::DefaultKeys();
            input_capture_.Cancel();
            dirty_ = true;
        }
        EndCard();
        // A click anywhere else stops waiting for a key; one on the waiting
        // keycap is taken when it is released.
        if (input_capture_.Waits(InputCapture::Kind::Key) && !clicked && !on_waiting && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            input_capture_.Cancel();
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
        view_opened_clean_ = !dirty_;
        input_capture_.Cancel();
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
        input_capture_.Cancel();
        if (view_opened_clean_ && dirty_ && config_ok_) Save();
    }

    // Profile `index` (below 0: built in, see Settings::PadProfileAt).
    const Settings::PadProfile& ProfileAt(int index) const { return draft_.PadProfileAt(index); }

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

    // Profile `index` as shown: a built-in layout (Default, Classic 1.1) for
    // the page's controller.
    Settings::PadProfile ShownProfile(int index) const
    {
        auto profile = ProfileAt(index);
        if (profile.follows) profile.inputs = platform::DefaultGamepadInputs(PageFamily(), *profile.follows);
        return profile;
    }

    std::string ProfileUsers(int index) const
    {
        std::string users;
        for (std::size_t slot = 0; slot < kGamepadSlots; ++slot)
            if (draft_.PadProfileIndex(slot) == index)
                users += (users.empty() ? "" : ", ")
                    + texts::T("settings.controls.gamepad.gamepad_number", {{"number", std::to_string(slot + 1)}});
        return users;
    }

    // A free name like "Profile 2".
    std::string NewProfileName() const
    {
        for (int n = int(draft_.pad_profiles.size()) + 1;; ++n)
        {
            const auto name = texts::T("settings.controls.gamepad.new_profile_name", {{"number", std::to_string(n)}});
            if (std::none_of(draft_.pad_profiles.begin(), draft_.pad_profiles.end(),
                             [&](const auto& profile) { return SamePadProfileName(profile.name, name); }))
                return name;
        }
    }

    // Selectables for the built-in profiles, then the named ones, of `kind`
    // (every profile, with its kind, when `kind` is empty), plus `current`;
    // returns the chosen index or kNoProfileChosen.
    static constexpr int kNoProfileChosen = -1000;
    int ProfileOptions(int current, const std::string& kind)
    {
        std::vector<int> indices{-1};
        for (std::size_t n = 0; n < Settings::BuiltinPadProfiles().size(); ++n)
            indices.push_back(Settings::BuiltinPadProfileIndex(n));
        for (int n = 0; n < int(draft_.pad_profiles.size()); ++n) indices.push_back(n);
        int chosen = kNoProfileChosen;
        for (const int n : indices)
        {
            const auto& profile = ProfileAt(n);
            // A built-in layout fits every controller.
            const bool fixed = !profile.follows;
            if (fixed && n != current && !kind.empty() && profile.controller != kind) continue;
            ImGui::PushID(n);
            std::string label = n < 0 ? texts::T("settings.controls.gamepad.built_in", {{"name", profile.name}})
                                      : profile.name;
            if (fixed && (kind.empty() || profile.controller != kind))
                label = texts::T("settings.controls.gamepad.for_kind",
                                 {{"profile", label}, {"kind", KindName(profile.controller)}});
            if (ImGui::Selectable(StableLabel(label.c_str(), "profile").c_str(), n == current)) chosen = n;
            ImGui::PopID();
        }
        return chosen;
    }

    // The controllers a profile kind is for (platform::GamepadKindName) in the
    // player's language: Nintendo, GameCube, Xbox and PlayStation.
    static std::string KindName(std::string_view kind)
    {
        if (kind == "nintendo" || kind == "gamecube") return platform::GamepadKindName(kind);
        return texts::T("settings.controls.gamepad.xbox_and_playstation");
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
                    if (pressed && !held[direction] && input_capture_.Waits(InputCapture::Kind::Pad)) CaptureGamepadInput(input);
                    held[direction] = pressed;
                    TrackPadInput(event.gaxis.which, input, pressed);
                }
            }
        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                            || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        {
            auto& held = trigger_held_[event.gaxis.which][event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
            const bool pressed = event.gaxis.value > platform::kGamepadTriggerPressed;
            if (pressed && !held && input_capture_.Waits(InputCapture::Kind::Pad))
                CaptureGamepadInput(platform::kGamepadTriggerBase + event.gaxis.axis);
            held = pressed;
            TrackPadInput(event.gaxis.which, platform::kGamepadTriggerBase + event.gaxis.axis, pressed);
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
            TrackPadInput(event.gbutton.which, event.gbutton.button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
            for (auto it = pad_inputs_held_.begin(); it != pad_inputs_held_.end();)
                it = it->first == event.gdevice.which ? pad_inputs_held_.erase(it) : std::next(it);
        if (!input_capture_.Waits(InputCapture::Kind::Pad)) return;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) CaptureGamepadInput(event.gbutton.button);
        else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) input_capture_.Cancel();
    }

    // Gamepad buttons and triggers held down, to keep a captured press away
    // from the launcher's navigation until it is released.
    void TrackPadInput(SDL_JoystickID pad, platform::GamepadInput input, bool down)
    {
        if (down) pad_inputs_held_.insert({pad, input});
        else pad_inputs_held_.erase({pad, input});
    }

    // The other actions of `bindings` that `input` presses too.
    static std::string SharedPadActions(const platform::GamepadBindings& bindings, std::size_t action,
                                        platform::GamepadInput input)
    {
        std::string actions;
        for (std::size_t other = 0; other < GamepadActionCount; ++other)
            if (other != action && platform::GamepadBindingHas(bindings[other], input))
                actions += (actions.empty() ? "" : ", ") + std::string(PadActionName(other));
        return actions;
    }

    // A press while an input waits: it takes that slot. An input may press
    // several actions at once (Default's Start presses 1 and +); the page
    // warns about it.
    void CaptureGamepadInput(platform::GamepadInput input)
    {
        if (!input_capture_.Waits(InputCapture::Kind::Pad)) return;
        const int action = input_capture_.action, slot = input_capture_.slot;
        input_capture_.Cancel();
        pad_capture_release_ = true;
        // (The profile edited may be gone: Revert reloads the list.)
        if (action < 0 || edit_profile_ < 0 || std::size_t(edit_profile_) >= draft_.pad_profiles.size()
            || platform::GamepadInputName(input).empty())
            return;
        auto& profile = draft_.pad_profiles[std::size_t(edit_profile_)];
        const std::string name = PadInputLabel(input, PageType());
        auto bindings = platform::ParseGamepadBindings(profile.inputs);
        auto& mine = bindings[std::size_t(action)];
        if (platform::GamepadBindingHas(mine, input)) return;
        if (std::size_t(slot) < platform::GamepadBindingSize(mine)) mine[std::size_t(slot)] = input;
        else platform::AddGamepadInput(mine, input);
        profile.inputs[std::size_t(action)] = platform::FormatGamepadBinding(mine);
        dirty_ = true;
        const auto shared = SharedPadActions(bindings, std::size_t(action), input);
        Notice(shared.empty()
                   ? texts::T("settings.controls.gamepad.now_presses",
                              {{"button", name}, {"action", PadActionName(std::size_t(action))}})
                   : texts::T("settings.controls.gamepad.now_presses_shared",
                              {{"button", name}, {"action", PadActionName(std::size_t(action))}, {"others", shared}}),
               shared.empty() ? NoticeKind::Info : NoticeKind::Warning);
    }

    // A combo choosing the profile of gamepad `slot`: Default and the
    // profiles for its connected controller (all without one).
    void ProfileCombo(const char* id, std::size_t slot, float width)
    {
        ImGui::SetNextItemWidth(width);
        const int current = draft_.PadProfileIndex(slot);
        const auto kind = SlotKind(slot);
        const auto& profile = ProfileAt(current);
        const bool mismatch = !profile.follows && !kind.empty() && profile.controller != kind;
        if (ImGui::BeginCombo(id, profile.name.c_str()))
        {
            const int chosen = ProfileOptions(current, kind);
            if (chosen != kNoProfileChosen)
            {
                draft_.pad_profile_names[slot] = ProfileAt(chosen).name;
                dirty_ = true;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen(id))
            ImGui::SetTooltip("%s", mismatch ? texts::T("settings.controls.gamepad.made_for",
                                                        {{"name", profile.name},
                                                         {"kind", KindName(profile.controller)}})
                                                   .c_str()
                                             : texts::T("settings.controls.gamepad.profile_tooltip"));
    }

    void EditProfile(int index)
    {
        edit_profile_ = index;
        renaming_ = confirm_delete_ = false;
        input_capture_.Cancel();
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
        copy.follows.reset();
        profiles.push_back(copy);
        EditProfile(int(profiles.size()) - 1);
        dirty_ = true;
        return true;
    }

    // A built-in profile never changes: a change to it goes into a copy,
    // which the page's gamepad plays with from then on. Null when all
    // profile slots are taken.
    Settings::PadProfile* OwnCopyForChange()
    {
        const auto builtin = ProfileAt(edit_profile_).name;
        if (!CopyShownProfile())
        {
            Notice(texts::T("settings.controls.gamepad.builtin_full", {{"name", builtin}}), NoticeKind::Warning);
            return nullptr;
        }
        auto& copy = draft_.pad_profiles.back();
        std::string user = texts::T("settings.controls.gamepad.copy_unassigned");
        if (pad_slot_ < kGamepadSlots)
        {
            draft_.pad_profile_names[pad_slot_] = copy.name;
            user = texts::T("settings.controls.gamepad.copy_assigned", {{"number", std::to_string(pad_slot_ + 1)}});
        }
        Notice(texts::T("settings.controls.gamepad.copy_made", {{"name", builtin}, {"copy", copy.name}}) + " " + user,
               NoticeKind::Info);
        return &copy;
    }

    void ProfileCard()
    {
        auto& profiles = draft_.pad_profiles;
        const bool builtin = edit_profile_ < 0;
        const std::string assigns = texts::T("settings.controls.gamepad.profile.about",
                                             {{"number", std::to_string(pad_slot_ + 1)}});
        BeginCard("##profile", texts::T("settings.controls.gamepad.profile.title"), assigns.c_str(), Icon::Gamepad);
        if (BeginRows("##profile_rows"))
        {
            Row(texts::T("settings.controls.gamepad.profile.edit"), nullptr);
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
                        Notice(texts::T("settings.controls.gamepad.profile.invalid_name"), NoticeKind::Warning);
                    else if (taken)
                        Notice(texts::T("settings.controls.gamepad.profile.name_taken", {{"name", name}}),
                               NoticeKind::Warning);
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
                if (chosen != kNoProfileChosen) EditProfile(chosen);
                ImGui::EndCombo();
            }
            EndRows();
        }
        // Which controller the page follows.
        const std::string kind_name = KindName(PageKind());
        const std::string controller = pad_slot_ < gamepad_names_.size()
            ? texts::T("settings.controls.gamepad.profile.connected",
                       {{"controller", gamepad_names_[pad_slot_]}, {"kind", kind_name}})
            : texts::T("settings.controls.gamepad.profile.not_connected",
                       {{"number", std::to_string(pad_slot_ + 1)}, {"kind", kind_name}});
        TextColored(CurrentFonts().caption, color::muted, controller.c_str());
        const auto users = ProfileUsers(edit_profile_);
        const std::string used = users.empty()
            ? std::string(texts::T("settings.controls.gamepad.profile.unused"))
            : texts::T("settings.controls.gamepad.profile.used_by", {{"users", users}});
        TextColored(CurrentFonts().caption, color::muted,
                    (builtin ? texts::T("settings.controls.gamepad.profile.builtin_copy") + std::string(" ") + used
                             : used)
                        .c_str());
        ImGui::Dummy(Dp(0, 4));
        if (SecondaryButton("##new_profile", texts::T("settings.controls.gamepad.profile.new"), Dp(110, 36),
                            profiles.size() < kMaxGamepadProfiles, Icon::Copy)
            && CopyShownProfile())
            Notice(texts::T("settings.controls.gamepad.profile.new_made", {{"name", profiles.back().name}}),
                   NoticeKind::Info);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", profiles.size() < kMaxGamepadProfiles
                                        ? texts::T("settings.controls.gamepad.profile.new_tooltip")
                                        : texts::T("settings.controls.gamepad.profile.new_full_tooltip"));
        ImGui::SameLine(0, Dp(8));
        if (SecondaryButton("##rename_profile", texts::T("settings.controls.gamepad.profile.rename"), Dp(120, 36),
                            !builtin && !renaming_, Icon::None))
        {
            renaming_ = rename_focus_ = true;
            confirm_delete_ = false;
            rename_buffer_ = profiles[std::size_t(edit_profile_)].name;
        }
        ImGui::SameLine(0, Dp(8));
        // Deleting asks once more; gamepads using the profile play with Default.
        if (SecondaryButton("##delete_profile",
                            confirm_delete_ ? texts::T("settings.controls.gamepad.profile.really_delete")
                                            : texts::T("settings.controls.gamepad.profile.delete"),
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
                Notice(texts::T("settings.controls.gamepad.profile.deleted",
                                {{"name", removed}, {"default", kDefaultGamepadProfile}}),
                       NoticeKind::Info);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (builtin ? texts::T("settings.controls.gamepad.profile.builtin_delete_tooltip")
                                     : confirm_delete_
                                         ? texts::T("settings.controls.gamepad.profile.confirm_delete_tooltip",
                                                    {{"default", kDefaultGamepadProfile}})
                                         : texts::T("settings.controls.gamepad.profile.delete_tooltip"))
                                        .c_str());
        else if (confirm_delete_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            confirm_delete_ = false;
        EndCard();
    }

    void GamepadView()
    {
        if (SecondaryButton("##back", texts::T("settings.controls.back_to_players"), Dp(124, 36), true, Icon::Back))
        { CloseGamepadView(); return; }
        ImGui::Dummy(Dp(0, 6));
        PageHeader(texts::T("settings.controls.gamepad.title"), texts::T("settings.controls.gamepad.about"));
        if (edit_profile_ >= int(draft_.pad_profiles.size())
            || edit_profile_ < Settings::BuiltinPadProfileIndex(Settings::BuiltinPadProfiles().size() - 1))
            edit_profile_ = -1;
        ProfileCard();
        // A built-in profile never changes: its first change makes an own copy.
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
                    TextColored(CurrentFonts().label, color::text, PadActionName(n));
                    if (const char* note = PadActionAbout(n))
                    {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
                        TextColored(CurrentFonts().caption, color::muted, note);
                    }
                    ImGui::TableSetColumnIndex(1);
                    const auto used = platform::GamepadBindingSize(bindings[n]);
                    for (int slot = 0; slot < int(platform::kGamepadBindingInputs); ++slot)
                    {
                        // The inputs, then a slot to add one more.
                        if (std::size_t(slot) > used) break;
                        const auto input = bindings[n][std::size_t(slot)];
                        const bool waiting = input_capture_.WaitsFor(InputCapture::Kind::Pad, int(n), slot);
                        const std::string label = waiting ? texts::T("settings.controls.gamepad.press_button")
                            : input != platform::kGamepadNone ? PadInputLabel(input, type)
                            : slot == 0 ? texts::T("settings.controls.pad.not_set") : "+";
                        if (slot) ImGui::SameLine(0, Dp(6));
                        ImGui::PushID(slot);
                        if (KeyCapButton("##input", label.c_str(), waiting) && (waiting || editable()))
                        {
                            clicked = true;
                            if (waiting) input_capture_.Cancel();
                            else input_capture_.Wait(InputCapture::Kind::Pad, int(n), slot);
                        }
                        if (ImGui::IsItemHovered() && !waiting)
                            ImGui::SetTooltip("%s", input != platform::kGamepadNone
                                                        ? texts::T("settings.controls.gamepad.change_tooltip")
                                                    : slot == 0 ? texts::T("settings.controls.gamepad.none_tooltip")
                                                                : texts::T("settings.controls.gamepad.add_tooltip"));
                        ImGui::PopID();
                    }
                    if (used > 0)
                    {
                        // Removes the last button, or the only one (no button then).
                        ImGui::SameLine(0, Dp(6));
                        if (SecondaryButton("##remove", "", Dp(30, 30), true, Icon::Error))
                            if (auto* target = editable())
                            {
                                auto binding = bindings[n];
                                binding[used - 1] = platform::kGamepadNone;
                                target->inputs[n] = platform::FormatGamepadBinding(binding);
                                dirty_ = true;
                            }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", used > 1
                                                        ? texts::T("settings.controls.gamepad.remove_last_tooltip")
                                                        : texts::T("settings.controls.gamepad.remove_only_tooltip"));
                    }
                    // Inputs that press other actions too.
                    std::string shared;
                    for (std::size_t k = 0; k < used; ++k)
                    {
                        const auto others = SharedPadActions(bindings, n, bindings[n][k]);
                        if (!others.empty())
                            shared += (shared.empty() ? "" : "; ")
                                + texts::T("settings.controls.gamepad.also_presses",
                                           {{"button", PadInputLabel(bindings[n][k], type)}, {"others", others}});
                    }
                    if (!shared.empty())
                    {
                        ImGui::SameLine(0, Dp(10));
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(7));
                        TextColored(CurrentFonts().caption, color::warning, shared.c_str());
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", texts::T("settings.controls.gamepad.both_tooltip"));
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        };
        BeginCard("##pad_remote", texts::T("settings.controls.gamepad.remote.title"),
                  texts::T("settings.controls.gamepad.remote.about"), Icon::Gamepad);
        rows(GamepadActionA, GamepadActionC);
        EndCard();
        BeginCard("##pad_nunchuk", texts::T("settings.controls.gamepad.nunchuk.title"),
                  texts::T("settings.controls.gamepad.nunchuk.about"), Icon::Gamepad);
        const auto sticks = profile.sticks;
        const bool gyro = profile.gyro_pointer;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 6));
        if (ImGui::BeginTable("##pad_sticks", 2))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, Dp(220));
            ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthStretch);
            KeyRow(texts::T("settings.controls.gamepad.nunchuk.stick"), {NunchukStickName(profile)}, nullptr,
                   texts::T("settings.controls.gamepad.nunchuk.stick_note"));
            KeyRow(texts::T("settings.controls.pad.pointer"), {PointerName(profile)}, nullptr,
                   texts::T("settings.controls.gamepad.nunchuk.pointer_note"));
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        if (BeginRows("##pad_swap_rows"))
        {
            Row(texts::T("settings.controls.gamepad.sticks.name"), texts::T("settings.controls.gamepad.sticks.about"));
            int stick_choice = int(sticks);
            if (Segmented("##sticks", &stick_choice,
                          {texts::T("settings.controls.gamepad.sticks.nunchuk_left"),
                           texts::T("settings.controls.gamepad.sticks.nunchuk_right"),
                           texts::T("settings.controls.gamepad.sticks.shared")},
                          ImGui::GetContentRegionAvail().x)
                && stick_choice != int(sticks))
                if (auto* target = editable())
                {
                    target->sticks = GamepadSticks(stick_choice);
                    dirty_ = true;
                }
            // Gyro pointer (issue #14).
            Row(texts::T("settings.controls.gamepad.gyro_pointer.name"),
                texts::T("settings.controls.gamepad.gyro_pointer.about"));
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
                Row(texts::T("settings.controls.gamepad.gyro_speed.name"),
                    texts::T("settings.controls.gamepad.gyro_speed.about"));
                int speed = profile.gyro_speed;
                if (PercentSlider("##gyro_speed", &speed, ImGui::GetContentRegionAvail().x, nullptr, kMinGyroSpeed,
                                  kMaxGyroSpeed)
                    && speed != profile.gyro_speed)
                    if (auto* target = editable())
                    {
                        target->gyro_speed = speed;
                        dirty_ = true;
                    }
                EndSubRows();
            }
            // A stick that neither moves the Nunchuk stick nor points (the
            // right one with shared sticks, the one the gyro frees) can press
            // the D-pad.
            if (gyro || sticks == GamepadSticks::Shared)
            {
                const int free_stick = sticks == GamepadSticks::Swapped ? platform::LeftStickUp : platform::RightStickUp;
                bool stick_dpad = true;
                for (int n = 0; n < 4; ++n)
                {
                    const auto& binding = bindings[GamepadActionUp + std::size_t(n)];
                    const auto input = platform::kGamepadStickBase + free_stick + n;
                    stick_dpad = stick_dpad && platform::GamepadBindingHas(binding, input);
                }
                Row(free_stick == platform::LeftStickUp ? texts::T("settings.controls.gamepad.stick_dpad.left")
                                                        : texts::T("settings.controls.gamepad.stick_dpad.right"),
                    texts::T("settings.controls.gamepad.stick_dpad.about"));
                if (Toggle("##stick_dpad", &stick_dpad))
                    if (auto* target = editable())
                    {
                        SetStickDpad(*target, free_stick, stick_dpad);
                        dirty_ = true;
                    }
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
        const int nunchuk_stick = sticks == GamepadSticks::Swapped ? platform::RightStickUp : platform::LeftStickUp;
        const int pointer_stick = sticks == GamepadSticks::Split ? platform::RightStickUp : platform::LeftStickUp;
        std::string stick_note;
        if (stick_bound(nunchuk_stick))
            stick_note = nunchuk_stick == platform::LeftStickUp
                ? texts::T("settings.controls.gamepad.stick_bound.left_nunchuk")
                : texts::T("settings.controls.gamepad.stick_bound.right_nunchuk");
        else if (!gyro && stick_bound(pointer_stick))
            stick_note = pointer_stick == platform::LeftStickUp
                ? texts::T("settings.controls.gamepad.stick_bound.left_pointer")
                : texts::T("settings.controls.gamepad.stick_bound.right_pointer");
        if (!stick_note.empty()) TextWrappedColored(CurrentFonts().caption, color::warning, stick_note.c_str());
        EndCard();

        // Dead zones and trigger press point (issue #62), tried live on the
        // page's gamepad.
        BeginCard("##pad_deadzones", texts::T("settings.controls.gamepad.dead_zones.title"),
                  texts::T("settings.controls.gamepad.dead_zones.about"), Icon::Sliders);
        if (BeginRows("##pad_deadzone_rows"))
        {
            Row(texts::T("settings.controls.gamepad.dead_zones.octagon.name"),
                texts::T("settings.controls.gamepad.dead_zones.octagon.about"));
            bool octagon = profile.octagon_stick;
            if (Toggle("##octagon_stick", &octagon))
                if (auto* target = editable())
                {
                    target->octagon_stick = octagon;
                    dirty_ = true;
                }
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
            slider(texts::T("settings.controls.gamepad.dead_zones.inner.name"),
                   texts::T("settings.controls.gamepad.dead_zones.inner.about"), "##inner_deadzone",
                   profile.inner_deadzone, kMinInnerDeadzone, kMaxInnerDeadzone, &Settings::PadProfile::inner_deadzone);
            slider(texts::T("settings.controls.gamepad.dead_zones.outer.name"),
                   texts::T("settings.controls.gamepad.dead_zones.outer.about"), "##outer_deadzone",
                   profile.outer_deadzone, kMinOuterDeadzone, kMaxOuterDeadzone, &Settings::PadProfile::outer_deadzone);
            slider(texts::T("settings.controls.gamepad.dead_zones.trigger.name"),
                   texts::T("settings.controls.gamepad.dead_zones.trigger.about"), "##trigger_press",
                   profile.trigger_press, kMinTriggerPress, kMaxTriggerPress, &Settings::PadProfile::trigger_press);
            EndRows();
        }
        DeadzonePreview(profile);
        // Defaults of the controller shown above.
        const auto defaults = platform::DefaultGamepadInputs(PageFamily());
        const auto& default_profile = Settings::DefaultPadProfile();
        const bool changed = platform::ParseGamepadBindings(profile.inputs) != platform::ParseGamepadBindings(defaults)
            || sticks != default_profile.sticks || profile.inner_deadzone != default_profile.inner_deadzone
            || profile.outer_deadzone != default_profile.outer_deadzone
            || profile.trigger_press != default_profile.trigger_press
            || profile.octagon_stick != default_profile.octagon_stick || profile.gyro_pointer != default_profile.gyro_pointer || profile.gyro_speed != default_profile.gyro_speed;
        ImGui::Dummy(Dp(0, 4));
        // (A built-in profile is not changed: it stays as it is made.)
        if (SecondaryButton("##reset_pad", texts::T("settings.controls.gamepad.reset"), Dp(180, 36), changed && !builtin,
                            Icon::Refresh))
        {
            profile.inputs = defaults;
            profile.sticks = default_profile.sticks;
            profile.inner_deadzone = default_profile.inner_deadzone;
            profile.outer_deadzone = default_profile.outer_deadzone;
            profile.trigger_press = default_profile.trigger_press;
            profile.octagon_stick = default_profile.octagon_stick;
            profile.gyro_pointer = default_profile.gyro_pointer;
            profile.gyro_speed = default_profile.gyro_speed;
            input_capture_.Cancel();
            dirty_ = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", texts::T("settings.controls.gamepad.reset_tooltip",
                                             {{"default", kDefaultGamepadProfile}}).c_str());
        EndCard();
        // A click anywhere else stops waiting for a press.
        if (input_capture_.Waits(InputCapture::Kind::Pad) && !clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) input_capture_.Cancel();
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
            for (auto& binding : bindings) platform::RemoveGamepadInput(binding, input);
            const std::size_t action = GamepadActionUp + std::size_t(n);
            auto& dpad = bindings[action];
            // A full D-pad action gives up its last button for the stick.
            if (on && !platform::AddGamepadInput(dpad, input)) dpad.back() = input;
            else if (!on && dpad[0] == platform::kGamepadNone) dpad = defaults[action];
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
                               texts::T("settings.controls.gamepad.gyro_preview.connect",
                                        {{"number", std::to_string(pad_slot_ + 1)}})
                                   .c_str());
            return;
        }
        const std::string name = pad_slot_ < gamepad_names_.size()
            ? gamepad_names_[pad_slot_]
            : std::string(texts::T("settings.controls.gamepad.gyro_preview.this_controller"));
        if (!SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO))
        {
            TextWrappedColored(fonts.caption, color::warning,
                               (profile.sticks == GamepadSticks::Split
                                    ? texts::T("settings.controls.gamepad.gyro_preview.no_gyro_right", {{"name", name}})
                                    : texts::T("settings.controls.gamepad.gyro_preview.no_gyro_left", {{"name", name}}))
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", texts::T("settings.controls.gamepad.gyro_preview.click_tooltip"));
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
            ? texts::T("settings.controls.gamepad.gyro_preview.button_centres",
                       {{"button", PadInputLabel(recenter_binding[0], SDL_GetGamepadType(pad))}})
            : std::string(texts::T("settings.controls.gamepad.gyro_preview.click_centres"));
        TextWrappedColored(fonts.caption, color::muted,
                           (texts::T("settings.controls.gamepad.gyro_preview.turn", {{"name", name}}) + " " + centre)
                               .c_str());
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
    // pushed. A real Nunchuk's stick (octagon_stick) shows its octagonal gate
    // and the game's dead zone along both axes.
    void DeadzonePreview(const Settings::PadProfile& profile)
    {
        const auto bindings = platform::ParseGamepadBindings(profile.inputs);
        SDL_Gamepad* pad = pad_slot_ < pads_.size() ? pads_[pad_slot_] : nullptr;
        if (!pad)
        {
            TextWrappedColored(CurrentFonts().caption, color::muted,
                               texts::T("settings.controls.gamepad.dead_zones.connect",
                                        {{"number", std::to_string(pad_slot_ + 1)}})
                                   .c_str());
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
        const auto stick = [&](float left, bool left_stick, bool nunchuk, bool pointer) {
            const ImVec2 c{left + radius, origin.y + radius};
            const auto x_axis = left_stick ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
            const auto y_axis = left_stick ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
            const char* stick_name = left_stick ? texts::T("settings.controls.pad.left_stick")
                                                : texts::T("settings.controls.pad.right_stick");
            float x = axis(x_axis), y = axis(y_axis);
            if (const float length = std::hypot(x, y); length > 1.0f) { x /= length; y /= length; }
            // Hairlines as thin rectangles: SDL's software renderer drops parts of long lines.
            const float hair = std::max(1.0f, Dp(1));
            if (nunchuk && profile.octagon_stick)
            {
                const auto counts = platform::OctagonNunchukStickCounts({x, -y});
                const float pushed = platform::NunchukStickPushed(counts);
                const float unit = radius / platform::kNunchukGateCounts;
                ImVec2 corners[8];
                for (int n = 0; n < 8; ++n)
                    corners[n] = {c.x + radius * std::cos(n * 0.785398163f), c.y + radius * std::sin(n * 0.785398163f)};
                draw->AddConvexPolyFilled(corners, 8, Col(color::field));
                // The game's dead zone along each axis, up to the gate's edge.
                const float band = unit * platform::kNunchukStickDeadCounts, reach = radius - band * 0.4142f;
                draw->AddRectFilled({c.x - reach, c.y - band}, {c.x + reach, c.y + band}, Col(color::dim, 0.30f));
                draw->AddRectFilled({c.x - band, c.y - reach}, {c.x + band, c.y + reach}, Col(color::dim, 0.30f));
                draw->AddRectFilled({c.x - reach, c.y - hair * 0.5f}, {c.x + reach, c.y + hair * 0.5f}, Col(color::border));
                draw->AddRectFilled({c.x - hair * 0.5f, c.y - reach}, {c.x + hair * 0.5f, c.y + reach}, Col(color::border));
                draw->AddPolyline(corners, 8, Col(color::border), ImDrawFlags_Closed, Dp(1.5f));
                const ImVec4 dot = pushed <= 0.0f ? color::muted : pushed >= 1.0f ? color::accent_hi : color::accent;
                draw->AddCircleFilled({c.x + counts[0] * unit, c.y - counts[1] * unit}, Dp(6), Col(dot));
                centred(c.x, origin.y + radius * 2 + Dp(6), fonts.caption, color::muted,
                        texts::T("settings.controls.gamepad.dead_zones.stick_real_nunchuk", {{"stick", stick_name}}));
                centred(c.x, origin.y + radius * 2 + Dp(22), fonts.caption, pushed > 0 ? color::text : color::dim,
                        texts::T("settings.controls.gamepad.dead_zones.pushed",
                                 {{"percent", std::to_string(int(std::lround(pushed * 100)))}}));
                return;
            }
            const auto tilt = platform::GamepadStickTilt(pad, x_axis, y_axis, profile.inner_deadzone,
                                                         profile.outer_deadzone);
            const float pushed = std::min(1.0f, std::hypot(tilt.x, tilt.y));
            draw->AddCircleFilled(c, radius, Col(color::field), 48);
            draw->AddCircleFilled(c, radius, Col(color::accent, 0.10f), 48);
            draw->AddCircleFilled(c, radius * profile.outer_deadzone / 100.0f, Col(color::field), 48);
            draw->AddCircle(c, radius * profile.outer_deadzone / 100.0f, Col(color::accent, 0.55f), 48, Dp(1.5f));
            draw->AddRectFilled({c.x - radius, c.y - hair * 0.5f}, {c.x + radius, c.y + hair * 0.5f}, Col(color::border));
            draw->AddRectFilled({c.x - hair * 0.5f, c.y - radius}, {c.x + hair * 0.5f, c.y + radius}, Col(color::border));
            if (profile.inner_deadzone > 0)
                draw->AddCircleFilled(c, radius * profile.inner_deadzone / 100.0f, Col(color::dim, 0.45f), 32);
            draw->AddCircle(c, radius, Col(color::border), 48, Dp(1.5f));
            const ImVec4 dot = pushed <= 0.0f ? color::muted : pushed >= 1.0f ? color::accent_hi : color::accent;
            draw->AddCircleFilled({c.x + x * radius, c.y + y * radius}, Dp(6), Col(dot));
            bool buttons = false;
            for (const auto& binding : bindings)
                for (const auto input : binding)
                    buttons = buttons
                        || (platform::IsGamepadStickDirection(input)
                            && (input - platform::kGamepadStickBase >= platform::RightStickUp) != left_stick);
            const std::initializer_list<texts::Arg> stick_arg = {{"stick", stick_name}};
            const std::string name = nunchuk && pointer
                ? texts::T("settings.controls.gamepad.dead_zones.stick_nunchuk_pointer", stick_arg)
                : nunchuk ? texts::T("settings.controls.gamepad.dead_zones.stick_nunchuk", stick_arg)
                : pointer ? texts::T("settings.controls.gamepad.dead_zones.stick_pointer", stick_arg)
                : buttons ? texts::T("settings.controls.gamepad.dead_zones.stick_buttons", stick_arg)
                          : texts::T("settings.controls.gamepad.dead_zones.stick_unused", stick_arg);
            centred(c.x, origin.y + radius * 2 + Dp(6), fonts.caption, color::muted, name);
            centred(c.x, origin.y + radius * 2 + Dp(22), fonts.caption, pushed > 0 ? color::text : color::dim,
                    texts::T("settings.controls.gamepad.dead_zones.pushed",
                             {{"percent", std::to_string(int(std::lround(pushed * 100)))}}));
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
                    pressed ? color::accent : color::dim,
                    pressed ? texts::T("settings.controls.gamepad.dead_zones.trigger_on")
                            : texts::T("settings.controls.gamepad.dead_zones.trigger_off"));
        };
        float left = origin.x + Dp(4);
        const bool stick_pointer = !profile.gyro_pointer;
        stick(left, true, profile.sticks != GamepadSticks::Swapped, stick_pointer && profile.sticks != GamepadSticks::Split);
        left += radius * 2 + gap;
        stick(left, false, profile.sticks == GamepadSticks::Swapped, stick_pointer && profile.sticks == GamepadSticks::Split);
        left += radius * 2 + gap;
        trigger(left, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        left += bar_w + gap;
        trigger(left, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        ImGui::Dummy({left + bar_w - origin.x, radius * 2 + caption});
    }

    void ControlsMain()
    {
        PageHeader(texts::T("settings.controls.title"), texts::T("settings.controls.about"));
        PlayersCard();
        HotkeysCard();

        BeginCard("##controller", texts::T("settings.controls.test.title"), texts::T("settings.controls.test.about"),
                  Icon::Gamepad);
        if (!gamepad_)
            TextWrappedColored(CurrentFonts().body, color::muted, texts::T("settings.controls.test.none"));
        else
        {
            const char* name = SDL_GetGamepadName(gamepad_);
            Chip(name ? name : texts::T("settings.controls.test.connected"), color::accent, Icon::Gamepad);
            const float x = float(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX)) / 32768.0f;
            const float y = float(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY)) / 32768.0f;
            const std::string stick = texts::T("settings.controls.test.left_stick",
                                               {{"x", std::to_string(int(std::lround(x * 100)))},
                                                {"y", std::to_string(int(std::lround(y * 100)))}});
            TextColored(CurrentFonts().body, color::text, stick.c_str());
            const SDL_GamepadButton buttons[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
            const char* labels[] = {texts::T("settings.controls.test.south"), texts::T("settings.controls.test.east"),
                                    texts::T("settings.controls.test.west"), texts::T("settings.controls.test.north")};
            for (int i = 0; i < 4; ++i)
            {
                if (i) ImGui::SameLine(0, Dp(8));
                Chip(labels[i], SDL_GetGamepadButton(gamepad_, buttons[i]) ? color::accent : color::dim);
            }
        }
        EndCard();
    }

    // The Super Team ([gameplay] super_team, only in the settings file) needs a
    // Super Mario Strikers (GameCube) image ([game] gamecube_disc): one of the
    // game folder is taken when none is set.
    void PrefillGameCubeDisc()
    {
        if (!draft_.gamecube_disc.empty() || library_strikers_.empty()) return;
        const bool clean = !dirty_;
        draft_.gamecube_disc = SettingsPath(library_strikers_.front());
        dirty_ = true;
        if (clean) Save();
        if (draft_.tweaks[GameTweakIndex(GameTweak::SuperTeam)])
            Notice(texts::T("settings.gameplay.super_team.uses_found",
                            {{"file", PathUtf8(library_strikers_.front().filename())}}),
                   NoticeKind::Success);
    }

    // A choice from a list: a button with the chosen label that opens the
    // list, the chosen one ticked. Whether another was chosen.
    template <std::size_t Size>
    bool ChoiceButton(const char* id, std::string& value, const std::array<const char*, Size>& keys,
                      const char* const (&labels)[Size])
    {
        const int current = std::max(TrainingIndex(keys, value), 0);
        ImGui::PushID(id);
        if (SecondaryButton("##choice", labels[current], {ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()}))
            ImGui::OpenPopup("##choice_menu");
        const auto& fonts = CurrentFonts();
        const float width = Dp(220);
        bool changed = false;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, Dp(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(10));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(color::card, color::window, 0.25f));
        // A long list (the items) scrolls instead of leaving the window.
        ImGui::SetNextWindowSizeConstraints({0, 0}, {FLT_MAX, ImGui::GetMainViewport()->WorkSize.y * 0.7f});
        if (ImGui::BeginPopup("##choice_menu"))
        {
            auto* draw = ImGui::GetWindowDrawList();
            for (int n = 0; n < int(Size); ++n)
            {
                ImGui::PushID(n);
                const ImVec2 pos = ImGui::GetCursorScreenPos(), size{width, Dp(36)};
                const bool pressed = ImGui::InvisibleButton("##row", size);
                const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered(), 14.0f);
                if (hover > 0.0f)
                    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, Col(color::text, 0.06f * hover), Dp(7));
                const bool chosen = n == current;
                const ImU32 ink = Col(chosen ? color::accent : Mix(Mix(color::muted, color::text, 0.7f), color::text, hover));
                DrawLabel(draw, fonts.label, {pos.x + Dp(12), pos.y + (size.y - FontSize(fonts.label)) * 0.5f}, ink,
                          labels[n]);
                if (chosen) DrawIcon(draw, Icon::Check, {pos.x + width - Dp(18), pos.y + size.y * 0.5f}, Dp(14), ink);
                if (pressed)
                {
                    changed = !chosen;
                    value = keys[std::size_t(n)];
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        ImGui::PopID();
        return changed;
    }

    void TrainingRows()
    {
        Row(texts::T("settings.gameplay.training.item.name"), texts::T("settings.gameplay.training.item.about"));
        if (ChoiceButton("training_item", draft_.training_item, kTrainingItems, kTrainingItemNames)) dirty_ = true;
        Row(texts::T("settings.gameplay.training.captain.name"), texts::T("settings.gameplay.training.captain.about"));
        if (ChoiceButton("training_captain", draft_.training_captain, kTrainingCaptains, kCaptainNames)) dirty_ = true;
        Row(texts::T("settings.gameplay.training.stadium.name"), texts::T("settings.gameplay.training.stadium.about"));
        if (ChoiceButton("training_stadium", draft_.training_stadium, kTrainingStadiums, kStadiumNames)) dirty_ = true;
        Row(texts::T("settings.gameplay.training.side.name"), texts::T("settings.gameplay.training.side.about"));
        {
            static constexpr std::array<const char*, 2> keys{"home", "away"};
            const char* labels[] = {texts::T("settings.gameplay.training.side.home"),
                                    texts::T("settings.gameplay.training.side.away")};
            if (ChoiceButton("training_side", draft_.training_side, keys, labels)) dirty_ = true;
        }
        Row(texts::T("settings.gameplay.training.opponent_captain.name"),
            texts::T("settings.gameplay.training.opponent_captain.about"));
        if (ChoiceButton("training_away_captain", draft_.training_away_captain, kTrainingCaptains, kCaptainNames))
            dirty_ = true;
        Row(texts::T("settings.gameplay.training.opponent_fielders.name"),
            texts::T("settings.gameplay.training.opponent_fielders.about"));
        {
            static constexpr std::array<const char*, 5> keys{"0", "1", "2", "3", "4"};
            const char* labels[] = {texts::T("settings.gameplay.training.opponent_fielders.none"), "1", "2", "3",
                                    texts::T("settings.gameplay.training.opponent_fielders.all")};
            std::string value = std::to_string(draft_.training_away_fielders);
            if (ChoiceButton("training_away_fielders", value, keys, labels))
            {
                draft_.training_away_fielders = value[0] - '0';
                dirty_ = true;
            }
        }
        Row(texts::T("settings.gameplay.training.opponent_skill.name"),
            texts::T("settings.gameplay.training.opponent_skill.about"));
        if (ChoiceButton("training_away_skill", draft_.training_away_skill, kTrainingSkills, kSkillNames)) dirty_ = true;
        Row(texts::T("settings.gameplay.training.opponent_goalie.name"),
            texts::T("settings.gameplay.training.opponent_goalie.about"));
        if (Toggle("##training_away_goalie", &draft_.training_away_goalie)) dirty_ = true;
        Row(texts::T("settings.gameplay.training.skip_goals.name"),
            texts::T("settings.gameplay.training.skip_goals.about"));
        if (Toggle("##training_skip_goals", &draft_.training_skip_goals)) dirty_ = true;
        const std::string blocker = PlayBlocker();
        Row(texts::T("settings.gameplay.training.start.name"), texts::T("settings.gameplay.training.start.about"));
        if (SecondaryButton("start_training", texts::T("settings.gameplay.training.start.button"),
                            {ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()}, blocker.empty(), Icon::Play))
            training_requested_ = true;
        if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", blocker.c_str());
    }

    // What needs files, folders or the launcher: texture packs, Training (it
    // starts the game) and the replays. The other gameplay options are the
    // game's OPTIONS > GAMEPLAY; the Super Team and the development switch for
    // the menus are only in the settings file.
    void ExtrasPage()
    {
        PageHeader(texts::T("settings.extras.title"), texts::T("settings.extras.about"));

        ModsCard();

        BeginCard("##training", texts::T("settings.gameplay.training.title"), nullptr, Icon::Pitch);
        if (BeginRows("##training_rows"))
        {
            TrainingRows();
            EndRows();
        }
        EndCard();
    }

    // Replays: whether matches are kept ([replays] record), the newest ones
    // to watch again and their folder; a replay's match stats in place of
    // the list.
    void ReplaysPage()
    {
        if (replay_stats_open_)
        {
            ReplayStatsView();
            return;
        }
        PageHeader(texts::T("settings.replays.title"), texts::T("settings.replays.about"));
        ReplaysCard();
    }
    static constexpr std::size_t kReplaysListed = 1000;
    // The replays folder, read in the background every two seconds while the
    // card shows (each replay's header is read: not on the window's thread).
    struct ReplayFolderList
    {
        std::vector<replay::Listing> listings;
        std::size_t total = 0;
        bool folder_exists = false;
    };
    BackgroundJob<ReplayFolderList> replay_listing_;
    ReplayFolderList replays_;
    bool replays_listed_ = false;
    std::uint64_t replay_listed_at_ = 0; // SDL ticks of the last reading's start

    void ReplaysCard()
    {
        const auto& fonts = CurrentFonts();
        const fs::path folder = replay::ReplayFolder(base_);
        if (auto listed = replay_listing_.Take())
        {
            replays_ = std::move(*listed);
            replays_listed_ = true;
        }
        if (const auto now = SDL_GetTicks();
            !replay_listing_.Busy() && (!replay_listed_at_ || now - replay_listed_at_ >= 2000))
        {
            replay_listed_at_ = now;
            replay_listing_.Start([folder] {
                ReplayFolderList list;
                list.listings = replay::ListReplays(folder, kReplaysListed, &list.total);
                std::error_code error;
                list.folder_exists = fs::is_directory(folder, error);
                return list;
            });
        }
        const auto& listings = replays_.listings;
        BeginCard("##replays", nullptr);
        if (BeginRows("##replays_rows"))
        {
            Row(texts::T("settings.replays.record.name"), texts::T("settings.replays.record.about"));
            if (Toggle("##replays_record", &draft_.replays_record)) dirty_ = true;
            EndRows();
        }
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        // Every replay on one line, newest first, the line a button that starts
        // it; the list scrolls past six lines.
        if (!listings.empty())
        {
            const auto captain = [](int team) {
                return team >= 0 && team < int(std::size(kCaptainNames)) && kCaptainNames[team] ? kCaptainNames[team]
                                                                                              : "?";
            };
            const float line = Dp(46);
            const float height = line * float(std::min<std::size_t>(listings.size(), 6));
            ImGui::Dummy({0, Dp(6)});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
            ImGui::BeginChild("##replay_list", {width, height});
            auto* list = ImGui::GetWindowDrawList();
            const float row = ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0 ? Dp(8) : 0.0f);
            // Only the lines in view are laid out (all of them are listed).
            ImGuiListClipper clipper;
            clipper.Begin(int(listings.size()), line);
            while (clipper.Step())
            for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible)
            {
                const std::size_t n = std::size_t(visible);
                const auto& listing = listings[n];
                const auto& info = listing.info;
                const bool readable = listing.error.empty() && info.home_team >= 0;
                const bool can = listing.error.empty() && config_ok_ && !pending_replay_;
                std::string title, detail, names;
                if (readable)
                {
                    title = texts::T("settings.replays.score",
                                     {{"home", captain(info.home_team)}, {"away", captain(info.away_team)},
                                      {"home_score", std::to_string(info.score[0])},
                                      {"away_score", std::to_string(info.score[1])}});
                    detail = LocalTime(info.created) + "  \xC2\xB7  " + replay::MatchLength(info.steps);
                    if (info.training) detail += std::string("  \xC2\xB7  ") + texts::T("settings.replays.training");
                    if (!info.names[0].empty() || !info.names[1].empty())
                        names = (info.names[0].empty() ? std::string("?") : info.names[0]) + "  vs  "
                            + (info.names[1].empty() ? std::string("?") : info.names[1]);
                }
                else
                {
                    title = PathUtf8(listing.path.filename());
                    detail = listing.error.empty() ? LocalTime(info.created) : texts::T("settings.replays.unreadable");
                }
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(int(n));
                // The line watches the replay, its stats button (at its right
                // end) shows the match's stats.
                const float stats_size = Dp(34), stats_room = stats_size + Dp(10);
                const float watch_width = row - stats_room;
                ImGui::BeginDisabled(!can);
                const bool pressed = ImGui::InvisibleButton("##replay", {watch_width, line});
                ImGui::EndDisabled();
                const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                const float hover = Animate(ImGui::GetItemID(), hovered && can, 14.0f);
                const float middle = pos.y + line * 0.5f;
                if (n > 0)
                    // (A filled hairline: SDL's software renderer breaks up long thin lines.)
                    list->AddRectFilled({pos.x + Dp(10), pos.y}, {pos.x + row - Dp(10), pos.y + 1.0f},
                                        Col(color::border, 0.7f));
                if (hover > 0.0f)
                    list->AddRectFilled({pos.x, pos.y + Dp(2)}, {pos.x + row, pos.y + line - Dp(2)},
                                        Col(color::card_hover, hover), Dp(8));
                // A play badge that lights up as the line is pointed at.
                const ImVec2 badge{std::floor(pos.x + Dp(22)), std::floor(middle)};
                list->AddCircleFilled(badge, Dp(13), Col(Mix(color::field, color::accent, hover)), 32);
                DrawIcon(list, Icon::Play, {badge.x + Dp(1.5f), badge.y}, Dp(11),
                         Col(can ? Mix(color::accent, color::accent_ink, hover) : color::dim));
                // The captains' faces, from the game image, overlapping.
                float text_x = pos.x + Dp(48);
                if (readable)
                {
                    const float face = Dp(28);
                    const bool home = DrawCaptainFace(list, {text_x + face * 0.5f, middle}, face, info.home_team);
                    const bool away = DrawCaptainFace(list, {text_x + face * 1.25f, middle}, face, info.away_team, true);
                    if (home || away) text_x += face * 1.75f + Dp(10);
                }
                const ImVec2 detail_size = TextSize(fonts.caption, detail.c_str());
                const float detail_x = pos.x + row - stats_room - Dp(8) - detail_size.x;
                const std::string shown = Elide(fonts.label, title, std::max(Dp(40), detail_x - Dp(16) - text_x));
                const ImVec2 title_size = TextSize(fonts.label, shown.c_str());
                if (names.empty())
                    DrawLabel(list, fonts.label, {text_x, middle - title_size.y * 0.5f},
                              Col(readable ? color::text : color::muted), shown.c_str());
                else
                {
                    const std::string names_shown
                        = Elide(fonts.caption, names, std::max(Dp(40), detail_x - Dp(16) - text_x));
                    const float names_height = TextSize(fonts.caption, names_shown.c_str()).y;
                    const float top = middle - (title_size.y + names_height + Dp(1)) * 0.5f;
                    DrawLabel(list, fonts.label, {text_x, top}, Col(color::text), shown.c_str());
                    DrawLabel(list, fonts.caption, {text_x, top + title_size.y + Dp(1)}, Col(color::muted),
                              names_shown.c_str());
                }
                DrawLabel(list, fonts.caption, {detail_x, middle - detail_size.y * 0.5f},
                          Col(listing.error.empty() ? color::muted : color::warning), detail.c_str());
                // The stats button: only for replays with the match's stats.
                ImGui::SetCursorScreenPos({pos.x + row - stats_size - Dp(4), middle - stats_size * 0.5f});
                const bool has_stats = readable && info.stats;
                ImGui::BeginDisabled(!has_stats);
                const bool stats_pressed = ImGui::InvisibleButton("##stats", {stats_size, stats_size});
                ImGui::EndDisabled();
                const bool stats_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                const float stats_hover = Animate(ImGui::GetItemID(), stats_hovered && has_stats, 14.0f);
                const ImVec2 stats_center{std::floor(pos.x + row - stats_size * 0.5f - Dp(4)), std::floor(middle)};
                if (stats_hover > 0.0f)
                    list->AddCircleFilled(stats_center, stats_size * 0.5f, Col(color::accent, 0.16f * stats_hover), 32);
                DrawIcon(list, Icon::Stats, stats_center, Dp(16),
                         Col(has_stats ? Mix(color::muted, color::accent, stats_hover) : color::dim, has_stats ? 1.0f : 0.5f));
                if (stats_hovered)
                    ImGui::SetTooltip("%s", has_stats ? texts::T("settings.replays.stats.open")
                                                      : texts::T("settings.replays.stats.unavailable"));
                if (stats_pressed && has_stats) OpenReplayStats(listing.path);
                ImGui::SetCursorScreenPos({pos.x, pos.y + line});
                if (hovered && !stats_hovered)
                    ImGui::SetTooltip("%s", listing.error.empty()
                                                ? (texts::T("settings.replays.watch") + std::string("\n")
                                                   + PathUtf8(listing.path)).c_str()
                                                : (listing.error + "\n" + PathUtf8(listing.path)).c_str());
                if (pressed && can) AskReplay(listing.path);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        // Under it: how many there are and where, and the folder's button.
        ImGui::Dummy({0, Dp(6)});
        const char* open = texts::T("settings.replays.open_folder");
        const float open_width = GhostWidth(open, Icon::Folder);
        const ImVec2 footer = ImGui::GetCursorScreenPos();
        const float button = Dp(34);
        const std::string about = !replays_listed_ ? PathUtf8(folder) : listings.empty()
            ? std::string(texts::T("settings.replays.none"))
            : texts::Count("settings.replays.count", int(replays_.total)) + "  \xC2\xB7  " + PathUtf8(folder);
        const std::string about_shown = Elide(fonts.caption, about, width - open_width - Dp(16));
        const ImVec2 about_size = TextSize(fonts.caption, about_shown.c_str());
        DrawLabel(draw, fonts.caption, {footer.x, footer.y + (button - about_size.y) * 0.5f}, Col(color::muted),
                  about_shown.c_str());
        ImGui::SetCursorScreenPos({footer.x + width - open_width, footer.y});
        std::error_code error;
        if (GhostButton("open_replays", open, {open_width, button}, replays_.folder_exists, Icon::Folder))
            OpenPath(folder);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PathUtf8(folder).c_str());
        EndCard();
    }

    // ------------------------------------------------- the game's pictures
    // The captains' HUD icons and the sidekicks' pictures in their teams'
    // kits (launcher/game_pictures.h), from the game image in use: its two
    // bundles read once on a worker, each picture made into a texture the
    // first time it is drawn. Recharged ships none of them.
    struct PictureBundles
    {
        fs::path disc;
        std::vector<std::uint8_t> icons, sidekicks;
        std::string error;
    };
    BackgroundJob<std::shared_ptr<const PictureBundles>> picture_job_;
    std::shared_ptr<const PictureBundles> pictures_;
    fs::path pictures_disc_; // the image the bundles are (being) read from
    std::map<std::string, SDL_Texture*> picture_textures_; // by name; null: none
    static constexpr std::size_t kMostPictureBundle = 32u << 20;

    void ClearPictureTextures()
    {
        for (auto& [name, texture] : picture_textures_)
            if (texture) SDL_DestroyTexture(texture);
        picture_textures_.clear();
    }

    const PictureBundles* GamePictureBundles()
    {
        if (auto taken = picture_job_.Take())
        {
            pictures_ = std::move(*taken);
            if (pictures_ && !pictures_->error.empty())
                platform::SessionLogLine("Match stats: the game image's pictures can't be read: " + pictures_->error);
        }
        const fs::path disc = config_ok_ ? EffectiveLaunch().disc_path : fs::path();
        if (disc.empty()) return nullptr;
        if (disc != pictures_disc_)
        {
            pictures_disc_ = disc;
            pictures_.reset();
            ClearPictureTextures();
            picture_job_.Start([disc] {
                auto bundles = std::make_shared<PictureBundles>();
                bundles->disc = disc;
                try
                {
                    bundles->icons = ReadDiscFile(disc, kCaptainIconBundle, kMostPictureBundle);
                    bundles->sidekicks = ReadDiscFile(disc, kSidekickPictureBundle, kMostPictureBundle);
                }
                catch (const std::exception& error)
                {
                    bundles->error = error.what();
                }
                return std::shared_ptr<const PictureBundles>(std::move(bundles));
            });
        }
        return pictures_ && pictures_->disc == disc ? pictures_.get() : nullptr;
    }

    // A picture of the game image by its name (`sidekick`: of the sidekicks'
    // bundle); null while the bundles are read or when it has none.
    SDL_Texture* GamePicture(const std::string& name, bool sidekick)
    {
        if (name.empty() || !renderer_) return nullptr;
        const PictureBundles* bundles = GamePictureBundles();
        if (!bundles) return nullptr;
        if (const auto it = picture_textures_.find(name); it != picture_textures_.end()) return it->second;
        SDL_Texture* texture = nullptr;
        const auto& bytes = sidekick ? bundles->sidekicks : bundles->icons;
        Picture picture;
        if (DecodePicture(BundleEntry({bytes.data(), bytes.size()}, name), picture))
        {
            texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, picture.width,
                                        picture.height);
            if (texture && !SDL_UpdateTexture(texture, nullptr, picture.rgba.data(), picture.width * 4))
            {
                SDL_DestroyTexture(texture);
                texture = nullptr;
            }
            if (texture)
            {
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            }
        }
        picture_textures_[name] = texture;
        return texture;
    }

    static ImTextureID PictureId(SDL_Texture* texture)
    {
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
    }

    // A captain's face in a disc `size` across (`ring`: a ring in the card's
    // colour, for faces drawn over each other); false without the picture.
    bool DrawCaptainFace(ImDrawList* draw, ImVec2 center, float size, int team, bool ring = false)
    {
        SDL_Texture* texture = GamePicture(CaptainIconPath(team), false);
        if (!texture) return false;
        const float r = size * 0.5f;
        if (ring) draw->AddCircleFilled(center, r + Dp(2), Col(color::card), 32);
        draw->AddImageRounded(PictureId(texture), {center.x - r, center.y - r}, {center.x + r, center.y + r}, {0, 0},
                              {1, 1}, IM_COL32_WHITE, r);
        return true;
    }

    // A player of a side as a face: the captain's icon, a sidekick's picture
    // (its head, in the team's kit) or, for the goalie or without pictures,
    // initials on the side's colour.
    void DrawPlayerFace(ImDrawList* draw, ImVec2 center, float size, const SideReport& side, int player,
                        const ImVec4& tint)
    {
        const float r = size * 0.5f;
        if (player == 0 && DrawCaptainFace(draw, center, size, side.captain)) return;
        if (player >= 1 && player <= 3)
            if (SDL_Texture* texture
                = GamePicture(SidekickPicturePath(side.sidekicks[std::size_t(player - 1)], side.captain), true))
            {
                // The head: the upper middle of the 1:2 picture.
                draw->AddImageRounded(PictureId(texture), {center.x - r, center.y - r}, {center.x + r, center.y + r},
                                      {0.0f, 0.10f}, {1.0f, 0.60f}, IM_COL32_WHITE, r);
                return;
            }
        draw->AddCircleFilled(center, r, Col(Mix(color::field, tint, 0.35f)), 32);
        const std::string initials = NameInitials(PlayerName(side, player));
        const ImVec2 text = TextSize(CurrentFonts().caption, initials.c_str());
        DrawLabel(draw, CurrentFonts().caption, {center.x - text.x * 0.5f, center.y - text.y * 0.5f}, Col(color::text),
                  initials.c_str());
    }

    // ------------------------------------------------- a replay's match stats
    // A replay's match stats (#177), shown on the Replays page in place of
    // the list (its line's stats button; Back, Escape or B return): read and
    // worked out on a worker.
    struct ReplayStats
    {
        fs::path path;
        replay::FileInfo info;
        int stadium = -1;
        int limit_type = 0, game_time = 0, goal_limit = 0, skill = -1;
        bool online = false;
        MatchReport report;
        std::string error;
    };
    BackgroundJob<std::shared_ptr<const ReplayStats>> replay_stats_job_;
    std::shared_ptr<const ReplayStats> replay_stats_;
    bool replay_stats_open_ = false;
    int shot_filter_ = 0; // the shot map: 0 both sides, 1 home, 2 away

    void OpenReplayStats(const fs::path& path)
    {
        replay_stats_open_ = true;
        replay_stats_.reset();
        shot_filter_ = 0;
        replay_stats_job_.Start([path] {
            auto stats = std::make_shared<ReplayStats>();
            stats->path = path;
            std::vector<std::uint8_t> bytes, stream;
            auto setup = std::make_unique<replay::Setup>();
            if (replay::ReadReplayFile(path, bytes, stats->error)
                && replay::DecodeFile(bytes.data(), bytes.size(), stats->info, stream, stats->error)
                && replay::ReadSetup(stream.data(), stream.size(), *setup, stats->error))
            {
                stats->stadium = setup->stadium;
                stats->skill = setup->settings[0];
                stats->limit_type = setup->settings[1];
                stats->game_time = setup->settings[2];
                stats->goal_limit = setup->settings[3];
                stats->online = setup->online != 0;
                stats->report = BuildMatchReport(*setup);
                stats->error.clear();
            }
            return std::shared_ptr<const ReplayStats>(std::move(stats));
        });
    }

    void CloseReplayStats()
    {
        replay_stats_job_.Drop();
        replay_stats_.reset();
        replay_stats_open_ = false;
    }

    static const char* CaptainName(int team)
    {
        return team >= 0 && team < int(std::size(kCaptainNames)) && kCaptainNames[team] ? kCaptainNames[team] : "?";
    }

    // A player of a side by name: the captain, a sidekick, the goalie.
    static std::string PlayerName(const SideReport& side, int player)
    {
        if (player == 0) return CaptainName(side.captain);
        if (player >= 1 && player <= 3)
        {
            const int sidekick = side.sidekicks[std::size_t(player - 1)];
            return sidekick >= 0 && sidekick < int(std::size(kSidekickNames)) && kSidekickNames[sidekick]
                ? kSidekickNames[sidekick]
                : "?";
        }
        return texts::T("settings.replays.stats.players.goalie");
    }

    // Who played a side: their name online (the captain in replays from
    // before names), else the controllers (Player 1) or the CPU.
    static std::string SideLabel(const ReplayStats& stats, int side)
    {
        const SideReport& report = stats.report.sides[std::size_t(side)];
        if (!report.name.empty()) return report.name;
        if (stats.online) return CaptainName(report.captain);
        if (report.pads.empty()) return texts::T("settings.replays.stats.cpu");
        std::string label;
        for (const int pad : report.pads)
            label += (label.empty() ? "" : ", ")
                + texts::T("settings.replays.stats.player", {{"number", std::to_string(pad + 1)}});
        return label;
    }

    // An item as the game names it: its kind and how many (Banana ×3), a
    // captain's ability by the captain.
    static std::string ItemName(int type, int count)
    {
        // The captains' abilities (ePowerUpType 9 to 20) in eTeamID.
        static constexpr int kAbilityCaptains[] = {0, 5, 3, 1, 4, 2, 7, 6, 9, 10, 8, 11};
        if (type >= 9 && type <= 20)
            return texts::T("settings.replays.stats.items.captains_ability",
                            {{"captain", CaptainName(kAbilityCaptains[type - 9])}});
        for (std::size_t n = 0; n < kTrainingItemKinds.size(); ++n)
            if (kTrainingItemKinds[n].type == type && kTrainingItemKinds[n].count == count
                && !kTrainingItemKinds[n].giant && kTrainingItemNames[n])
                return kTrainingItemNames[n];
        for (std::size_t n = 0; n < kTrainingItemKinds.size(); ++n)
            if (kTrainingItemKinds[n].type == type && kTrainingItemKinds[n].count == 1 && kTrainingItemNames[n])
                return count > 1 ? texts::T("settings.replays.stats.items.count",
                                            {{"item", kTrainingItemNames[n]}, {"count", std::to_string(count)}})
                                 : std::string(kTrainingItemNames[n]);
        return texts::T("settings.replays.stats.items.unknown");
    }

    static ImVec4 SideTint(int side) { return side == 0 ? color::accent : color::info; }

    void ReplayStatsView()
    {
        const auto& fonts = CurrentFonts();
        if (auto taken = replay_stats_job_.Take()) replay_stats_ = std::move(*taken);
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 top = ImGui::GetCursorScreenPos();
        if (SecondaryButton("##stats_back", texts::T("settings.replays.stats.back"), Dp(124, 36), true, Icon::Back)
            || (CancelPressed() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)))
        {
            CloseReplayStats();
            return;
        }
        if (!replay_stats_)
        {
            ImGui::Dummy(Dp(0, 18));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            Spinner({at.x + Dp(12), at.y + Dp(12)}, Dp(9), Col(color::accent));
            DrawLabel(ImGui::GetWindowDrawList(), fonts.body, {at.x + Dp(32), at.y + Dp(3)}, Col(color::muted),
                      texts::T("settings.replays.stats.loading"));
            ImGui::Dummy(Dp(0, 24));
            return;
        }
        const ReplayStats& stats = *replay_stats_;
        if (!stats.error.empty())
        {
            ImGui::Dummy(Dp(0, 12));
            Callout(texts::T("settings.replays.stats.unreadable", {{"error", stats.error}}), color::danger, Icon::Error);
            return;
        }
        // Watch, at the right of Back.
        const char* watch = texts::T("settings.replays.stats.watch");
        const float watch_width = TextSize(fonts.body, watch).x + Dp(18 + 8 + 28);
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos({top.x + width - watch_width, top.y});
        if (PrimaryButton("##stats_watch", watch, {watch_width, Dp(36)}, config_ok_ && !pending_replay_, Icon::Play))
            AskReplay(stats.path);
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(Dp(0, 8));
        ReplayStatsHeader(stats);
        const MatchReport& report = stats.report;
        if (!report.has_stats)
        {
            Callout(texts::T("settings.replays.stats.older"), color::info, Icon::Info);
            return;
        }
        ReplayStatsSummary(stats);
        ReplayStatsGoals(stats);
        ReplayStatsTimeline(stats);
        ReplayStatsShots(stats);
        ReplayStatsItems(stats);
        ReplayStatsPlayers(stats);
    }

    // The match: both sides (who played, their captain's face and sidekicks),
    // the score, and when, where and by which rules it was played.
    void ReplayStatsHeader(const ReplayStats& stats)
    {
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        BeginCard("##stats_header", nullptr);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float face = Dp(64), portrait_w = Dp(30), portrait_h = Dp(60), gap = Dp(6);
        const float height = face + Dp(12) + portrait_h + Dp(10);
        const int winner = report.score[0] == report.score[1] ? -1 : report.score[0] > report.score[1] ? 0 : 1;
        for (int side = 0; side < 2; ++side)
        {
            const SideReport& team = report.sides[std::size_t(side)];
            const bool right = side == 1;
            const float x = right ? at.x + width - face : at.x;
            const ImVec2 center{x + face * 0.5f, at.y + face * 0.5f};
            draw->AddCircleFilled(center, face * 0.5f + Dp(3), Col(SideTint(side), 0.85f), 48);
            if (!DrawCaptainFace(draw, center, face, team.captain))
                DrawPlayerFace(draw, center, face, team, 0, SideTint(side));
            // Who played, then the captain (once when they are the same).
            const std::string label = Elide(fonts.heading, SideLabel(stats, side), width * 0.5f - face - Dp(70));
            const std::string captain = SideLabel(stats, side) == CaptainName(team.captain) ? "" : CaptainName(team.captain);
            const ImVec2 label_size = TextSize(fonts.heading, label.c_str());
            const ImVec2 captain_size = TextSize(fonts.body, captain.c_str());
            const float text_x = right ? x - Dp(14) - label_size.x : x + face + Dp(14);
            const float captain_x = right ? x - Dp(14) - captain_size.x : x + face + Dp(14);
            DrawLabel(draw, fonts.heading, {text_x, at.y + Dp(10)},
                      Col(winner == -1 || winner == side ? color::text : color::muted), label.c_str());
            DrawLabel(draw, fonts.body, {captain_x, at.y + Dp(14) + label_size.y}, Col(color::muted), captain.c_str());
            // The sidekicks in the team's kit.
            for (int slot = 0; slot < 3; ++slot)
            {
                const float px = right ? at.x + width - (3 - slot) * (portrait_w + gap) + gap
                                       : at.x + slot * (portrait_w + gap);
                const ImVec2 min{px, at.y + face + Dp(12)}, max{px + portrait_w, at.y + face + Dp(12) + portrait_h};
                const int sidekick = team.sidekicks[std::size_t(slot)];
                if (SDL_Texture* texture = GamePicture(SidekickPicturePath(sidekick, team.captain), true))
                    draw->AddImageRounded(PictureId(texture), min, max, {0, 0}, {1, 1}, IM_COL32_WHITE, Dp(6));
                else
                {
                    draw->AddRectFilled(min, max, Col(Mix(color::field, SideTint(side), 0.2f)), Dp(6));
                    const std::string initials = NameInitials(PlayerName(team, slot + 1));
                    const ImVec2 size = TextSize(fonts.caption, initials.c_str());
                    DrawLabel(draw, fonts.caption, {(min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f},
                              Col(color::muted), initials.c_str());
                }
                if (ImGui::IsMouseHoveringRect(min, max)) ImGui::SetTooltip("%s", PlayerName(team, slot + 1).c_str());
            }
        }
        // The score in the middle, the winner's number lit.
        const std::string home = std::to_string(report.score[0]), away = std::to_string(report.score[1]);
        const std::string dash = "\xE2\x80\x93";
        const float home_w = TextSize(fonts.title, home.c_str()).x, away_w = TextSize(fonts.title, away.c_str()).x;
        const float dash_w = TextSize(fonts.title, dash.c_str()).x, space = Dp(14);
        float sx = at.x + (width - home_w - dash_w - away_w - 2 * space) * 0.5f;
        const float sy = at.y + Dp(8);
        DrawLabel(draw, fonts.title, {sx, sy}, Col(winner == 0 ? color::text : color::muted), home.c_str());
        sx += home_w + space;
        DrawLabel(draw, fonts.title, {sx, sy}, Col(color::dim), dash.c_str());
        sx += dash_w + space;
        DrawLabel(draw, fonts.title, {sx, sy}, Col(winner == 1 ? color::text : color::muted), away.c_str());
        // When, where, how long and by which rules.
        std::vector<std::string> facts{LocalTime(stats.info.created)};
        if (stats.stadium >= 0 && stats.stadium < int(std::size(kStadiumNames)) && kStadiumNames[stats.stadium])
            facts.push_back(kStadiumNames[stats.stadium]);
        facts.push_back(texts::T("settings.replays.stats.played", {{"time", ClockText(report.length)}}));
        if (stats.info.training) facts.push_back(texts::T("settings.replays.training"));
        else if (stats.limit_type == 1 && stats.goal_limit > 0)
            facts.push_back(texts::T("settings.replays.stats.goal_limit", {{"goals", std::to_string(stats.goal_limit)}}));
        else if (stats.game_time > 0 && stats.game_time < 3600)
            facts.push_back(texts::T("settings.replays.stats.time_limit", {{"time", ClockText(float(stats.game_time))}}));
        if (stats.online) facts.push_back(texts::T("settings.replays.stats.online"));
        else if (stats.info.network) facts.push_back(texts::T("settings.replays.stats.lan"));
        const bool cpu = !stats.info.network && (report.sides[0].pads.empty() || report.sides[1].pads.empty());
        if (cpu && stats.skill >= 0 && stats.skill < int(std::size(kSkillNames)) && kSkillNames[stats.skill])
            facts.push_back(kSkillNames[stats.skill]);
        const float facts_room = width - 2 * (3 * (portrait_w + gap)) - Dp(24);
        float fy = sy + TextSize(fonts.title, home.c_str()).y + Dp(8);
        std::string line;
        const auto flush = [&] {
            const ImVec2 size = TextSize(fonts.caption, line.c_str());
            DrawLabel(draw, fonts.caption, {at.x + (width - size.x) * 0.5f, fy}, Col(color::muted), line.c_str());
            fy += size.y + Dp(3);
            line.clear();
        };
        for (const std::string& fact : facts)
        {
            const std::string longer = line.empty() ? fact : line + "  \xC2\xB7  " + fact;
            if (!line.empty() && TextSize(fonts.caption, longer.c_str()).x > facts_room) flush();
            line = line.empty() ? fact : line + "  \xC2\xB7  " + fact;
        }
        if (!line.empty()) flush();
        ImGui::Dummy({width, std::max(height, fy - at.y)});
        EndCard();
    }

    // The totals side by side: each row's numbers, the leader's lit, and a
    // bar from the middle for each side.
    void ReplayStatsSummary(const ReplayStats& stats)
    {
        static constexpr std::pair<StatRow, const char*> kRows[] = {
            {StatRow::Goals, "settings.replays.stats.rows.goals"},
            {StatRow::Shots, "settings.replays.stats.rows.shots"},
            {StatRow::ShotsOnGoal, "settings.replays.stats.rows.shots_on_goal"},
            {StatRow::Possession, "settings.replays.stats.rows.possession"},
            {StatRow::Passes, "settings.replays.stats.rows.passes"},
            {StatRow::PerfectPasses, "settings.replays.stats.rows.perfect_passes"},
            {StatRow::Interceptions, "settings.replays.stats.rows.interceptions"},
            {StatRow::Hits, "settings.replays.stats.rows.hits"},
            {StatRow::Steals, "settings.replays.stats.rows.steals"},
            {StatRow::Dekes, "settings.replays.stats.rows.dekes"},
            {StatRow::Items, "settings.replays.stats.rows.items"},
            {StatRow::Skillshots, "settings.replays.stats.rows.skillshots"},
            {StatRow::MegaStrikes, "settings.replays.stats.rows.mega_strikes"},
            {StatRow::Fouls, "settings.replays.stats.rows.fouls"},
        };
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        BeginCard("##stats_summary", texts::T("settings.replays.stats.summary"), nullptr, Icon::Stats);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        // The sides' names over their columns.
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            for (int side = 0; side < 2; ++side)
            {
                const std::string label = Elide(fonts.caption, SideLabel(stats, side), width * 0.3f);
                const ImVec2 size = TextSize(fonts.caption, label.c_str());
                DrawLabel(draw, fonts.caption, {side == 0 ? at.x : at.x + width - size.x, at.y},
                          Col(SideTint(side)), label.c_str());
            }
            ImGui::Dummy({width, Dp(20)});
        }
        const float row_height = Dp(38), bar = Dp(4), half = width * 0.5f;
        for (const auto& [which, text] : kRows)
        {
            const auto& values = report.rows[std::size_t(which)];
            const bool always = which == StatRow::Goals || which == StatRow::Shots || which == StatRow::Possession;
            if (!always && values[0] == 0 && values[1] == 0) continue;
            const bool percent = which == StatRow::Possession;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const int most = std::max(values[0], values[1]);
            const char* label = texts::T(text);
            const ImVec2 label_size = TextSize(fonts.body, label);
            DrawLabel(draw, fonts.body, {at.x + half - label_size.x * 0.5f, at.y + Dp(4)}, Col(color::muted), label);
            for (int side = 0; side < 2; ++side)
            {
                const int value = values[std::size_t(side)];
                const std::string number = std::to_string(value) + (percent ? "%" : "");
                const bool leads = value > 0 && value == most && values[0] != values[1];
                const ImVec2 size = TextSize(fonts.label, number.c_str());
                DrawLabel(draw, fonts.label, {side == 0 ? at.x : at.x + width - size.x, at.y + Dp(3)},
                          Col(leads ? SideTint(side) : color::text), number.c_str());
                // The bar from the middle out, as long as its share of the larger.
                const float share = most > 0 ? float(value) / float(most) : 0.0f;
                const float length = (half - Dp(6)) * share;
                const float y = at.y + row_height - bar - Dp(6);
                draw->AddRectFilled({at.x + Dp(0), y}, {at.x + width, y + bar}, Col(color::field), bar * 0.5f);
                if (length > 0.0f)
                {
                    const ImVec2 min = side == 0 ? ImVec2{at.x + half - Dp(2) - length, y} : ImVec2{at.x + half + Dp(2), y};
                    draw->AddRectFilled(min, {min.x + length, y + bar}, Col(SideTint(side), leads ? 1.0f : 0.45f),
                                        bar * 0.5f);
                }
            }
            ImGui::Dummy({width, row_height});
        }
        EndCard();
    }

    // The goals in order: when, who scored (and assisted), how, and the score
    // after it.
    void ReplayStatsGoals(const ReplayStats& stats)
    {
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        BeginCard("##stats_goals", texts::T("settings.replays.stats.goals.title"), nullptr, Icon::Target);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        if (report.goals.empty())
            TextColored(fonts.body, color::muted, texts::T("settings.replays.stats.goals.none"));
        const float line = Dp(40), face = Dp(28);
        for (std::size_t n = 0; n < report.goals.size(); ++n)
        {
            const GoalLine& goal = report.goals[n];
            const SideReport& team = report.sides[std::size_t(goal.side)];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float middle = at.y + line * 0.5f;
            if (n > 0) draw->AddRectFilled({at.x, at.y}, {at.x + width, at.y + 1.0f}, Col(color::border, 0.6f));
            const std::string time = ClockText(goal.seconds);
            const ImVec2 time_size = TextSize(fonts.label, time.c_str());
            DrawLabel(draw, fonts.label, {at.x, middle - time_size.y * 0.5f}, Col(color::muted), time.c_str());
            float x = at.x + Dp(56);
            draw->AddRectFilled({x - Dp(10), middle - Dp(12)}, {x - Dp(7), middle + Dp(12)}, Col(SideTint(goal.side)),
                                Dp(2));
            std::string who;
            if (goal.own_goal) who = texts::T("settings.replays.stats.goals.own_goal");
            else
            {
                DrawPlayerFace(draw, {x + face * 0.5f, middle}, face, team, goal.player, SideTint(goal.side));
                x += face + Dp(10);
                who = goal.player >= 0 ? PlayerName(team, goal.player) : CaptainName(team.captain);
            }
            std::vector<std::string> notes;
            if (goal.assist >= 0)
                notes.push_back(texts::T("settings.replays.stats.goals.assist", {{"name", PlayerName(team, goal.assist)}}));
            if (goal.kind == 6)
                notes.push_back(texts::Count("settings.replays.stats.goals.mega_strike", goal.count));
            else if (goal.kind == 1) notes.push_back(texts::T("settings.replays.stats.goals.one_timer"));
            else if (goal.kind == 2) notes.push_back(texts::T("settings.replays.stats.goals.skillshot"));
            else if (goal.kind == 3) notes.push_back(texts::T("settings.replays.stats.goals.deflection"));
            std::string note;
            for (const auto& part : notes) note += (note.empty() ? "" : "  \xC2\xB7  ") + part;
            const ImVec2 who_size = TextSize(fonts.label, who.c_str());
            DrawLabel(draw, fonts.label, {x, middle - who_size.y * 0.5f}, Col(color::text), who.c_str());
            if (!note.empty())
            {
                const ImVec2 note_size = TextSize(fonts.caption, note.c_str());
                DrawLabel(draw, fonts.caption, {x + who_size.x + Dp(10), middle - note_size.y * 0.5f},
                          Col(color::muted), note.c_str());
            }
            // The score after it, the scoring side's number lit.
            const std::string home = std::to_string(goal.score[0]), away = std::to_string(goal.score[1]);
            const std::string dash = "  \xE2\x80\x93  ";
            const float away_w = TextSize(fonts.label, away.c_str()).x, dash_w = TextSize(fonts.label, dash.c_str()).x;
            const float home_w = TextSize(fonts.label, home.c_str()).x;
            float sx = at.x + width - away_w - dash_w - home_w;
            const float sy = middle - TextSize(fonts.label, home.c_str()).y * 0.5f;
            DrawLabel(draw, fonts.label, {sx, sy}, Col(goal.side == 0 ? SideTint(0) : color::muted), home.c_str());
            sx += home_w;
            DrawLabel(draw, fonts.label, {sx, sy}, Col(color::dim), dash.c_str());
            sx += dash_w;
            DrawLabel(draw, fonts.label, {sx, sy}, Col(goal.side == 1 ? SideTint(1) : color::muted), away.c_str());
            ImGui::Dummy({width, line});
        }
        EndCard();
    }

    // The match from kickoff to its end: home's moments above the line,
    // away's below; a goal a ball, an item a diamond, a hit, a steal or a
    // foul a mark. Pointing at one tells what it was.
    void ReplayStatsTimeline(const ReplayStats& stats)
    {
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        BeginCard("##stats_timeline", texts::T("settings.replays.stats.timeline.title"), nullptr, Icon::Clipboard);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float height = Dp(118), axis = at.y + height * 0.5f - Dp(6), pad = Dp(10);
        const float length = std::max(report.length, 1.0f);
        const auto x_of = [&](float seconds) {
            return at.x + pad + (width - 2 * pad) * std::clamp(seconds / length, 0.0f, 1.0f);
        };
        draw->AddRectFilled({at.x + pad, axis - 1.0f}, {at.x + width - pad, axis + 1.0f}, Col(color::border));
        // A tick each minute (each five past ten minutes), with its time.
        const int step = length > 600.0f ? 300 : 60;
        for (int second = 0; second <= int(length); second += step)
        {
            const float x = x_of(float(second));
            draw->AddRectFilled({x - 0.5f, axis - Dp(4)}, {x + 0.5f, axis + Dp(4)}, Col(color::dim));
            const std::string label = ClockText(float(second));
            const ImVec2 size = TextSize(fonts.caption, label.c_str());
            DrawLabel(draw, fonts.caption, {std::clamp(x - size.x * 0.5f, at.x, at.x + width - size.x), at.y + height - size.y},
                      Col(color::dim), label.c_str());
        }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        std::string tip;
        float tip_distance = Dp(9);
        const auto consider = [&](ImVec2 point, const std::string& text) {
            const float d = std::hypot(mouse.x - point.x, mouse.y - point.y);
            if (d < tip_distance && ImGui::IsWindowHovered())
            {
                tip_distance = d;
                tip = text;
            }
        };
        const auto lane = [&](int side, float level) { return axis + (side == 0 ? -1.0f : 1.0f) * level; };
        for (const Moment& moment : report.moments)
        {
            if (moment.kind == Moment::Goal || moment.kind == Moment::Item) continue;
            const ImVec2 point{x_of(moment.seconds), lane(moment.side, Dp(10))};
            const ImU32 tint = Col(SideTint(moment.side), 0.55f);
            const char* what = moment.kind == Moment::Hit ? "settings.replays.stats.timeline.hit"
                : moment.kind == Moment::Steal            ? "settings.replays.stats.timeline.steal"
                                                          : "settings.replays.stats.timeline.foul";
            if (moment.kind == Moment::Foul)
                draw->AddRectFilled({point.x - Dp(2.5f), point.y - Dp(2.5f)}, {point.x + Dp(2.5f), point.y + Dp(2.5f)},
                                    Col(color::warning, 0.8f));
            else draw->AddRectFilled({point.x - 1.0f, point.y - Dp(4)}, {point.x + 1.0f, point.y + Dp(4)}, tint);
            consider(point, ClockText(moment.seconds) + "  " + texts::T(what) + "  \xC2\xB7  " + SideLabel(stats, moment.side));
        }
        for (const ItemUse& item : report.items)
        {
            const ImVec2 point{x_of(item.seconds), lane(item.side, Dp(24))};
            const float r = Dp(4.5f);
            const ImVec2 diamond[] = {{point.x, point.y - r}, {point.x + r, point.y}, {point.x, point.y + r}, {point.x - r, point.y}};
            draw->AddConvexPolyFilled(diamond, 4, Col(SideTint(item.side), 0.8f));
            consider(point, ClockText(item.seconds) + "  " + ItemName(item.type, item.count) + "  \xC2\xB7  "
                                 + PlayerName(report.sides[std::size_t(item.side)], item.player));
        }
        for (const GoalLine& goal : report.goals)
        {
            const ImVec2 point{x_of(goal.seconds), lane(goal.side, Dp(40))};
            draw->AddLine({point.x, axis}, point, Col(SideTint(goal.side), 0.5f), Dp(1.5f));
            draw->AddCircleFilled(point, Dp(8), Col(color::text), 24);
            draw->AddCircleFilled(point, Dp(6), Col(SideTint(goal.side)), 24);
            const SideReport& team = report.sides[std::size_t(goal.side)];
            const std::string who = goal.own_goal ? texts::T("settings.replays.stats.goals.own_goal")
                                                  : goal.player >= 0 ? PlayerName(team, goal.player) : "";
            consider(point, ClockText(goal.seconds) + "  " + texts::T("settings.replays.stats.timeline.goal") + "  "
                                + std::to_string(goal.score[0]) + "\xE2\x80\x93" + std::to_string(goal.score[1])
                                + (who.empty() ? std::string() : "  \xC2\xB7  " + who));
        }
        ImGui::Dummy({width, height});
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        EndCard();
    }

    // Where each shot was taken, on the field to scale: home's in the
    // accent, away's in blue, a filled dot for a shot that scored.
    void ReplayStatsShots(const ReplayStats& stats)
    {
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        BeginCard("##stats_shots", texts::T("settings.replays.stats.shots.title"),
                  texts::T("settings.replays.stats.shots.about"), Icon::Pitch);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const std::string home = SideLabel(stats, 0), away = SideLabel(stats, 1);
        Segmented("##shot_side", &shot_filter_, {texts::T("settings.replays.stats.shots.both"), home.c_str(), away.c_str()},
                  std::min(width, Dp(420)));
        ImGui::Dummy(Dp(0, 8));
        if (report.shots.empty() || report.field_x <= 0.0f || report.field_y <= 0.0f)
        {
            TextColored(fonts.body, color::muted, texts::T("settings.replays.stats.shots.none"));
            EndCard();
            return;
        }
        const float map_w = std::min(width, Dp(640));
        const float map_h = map_w * report.field_y / report.field_x;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 min{at.x + (width - map_w) * 0.5f, at.y}, max{min.x + map_w, min.y + map_h};
        const ImVec2 center{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
        const float sx = map_w * 0.5f / report.field_x, sy = map_h * 0.5f / report.field_y;
        const auto point = [&](float x, float y) { return ImVec2{center.x + x * sx, center.y - y * sy}; };
        const ImU32 lines = Col(color::text, 0.35f);
        const float corner = std::clamp(report.corner * sx, 0.0f, map_h * 0.5f);
        draw->AddRectFilled(min, max, Col(Mix(color::field, color::accent, 0.07f)), corner);
        draw->AddRect(min, max, lines, corner, 0, Dp(1.5f));
        draw->AddLine({center.x, min.y}, {center.x, max.y}, lines, Dp(1.2f));
        draw->AddCircle(center, map_h * 0.17f, lines, 48, Dp(1.2f));
        if (report.box_x > 0.0f && report.box_x < report.field_x && report.box_y > 0.0f && report.box_y < report.field_y)
            for (const float end : {-1.0f, 1.0f})
            {
                const ImVec2 a = point(end * report.field_x, report.box_y), b = point(end * report.box_x, -report.box_y);
                draw->AddRect({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}, lines,
                              0.0f, 0, Dp(1.2f));
            }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        std::string tip;
        float tip_distance = Dp(8);
        // The missed ones first, the goals over them.
        for (const bool scored : {false, true})
            for (const ShotSpot& shot : report.shots)
            {
                if (shot.scored != scored || (shot_filter_ == 1 && shot.side != 0) || (shot_filter_ == 2 && shot.side != 1))
                    continue;
                const ImVec2 p = point(std::clamp(shot.x, -report.field_x, report.field_x),
                                       std::clamp(shot.y, -report.field_y, report.field_y));
                const ImU32 tint = Col(SideTint(shot.side));
                const float r = shot.kind == ShotKind::MegaStrike ? Dp(8) : Dp(5.5f);
                if (scored)
                {
                    draw->AddCircleFilled(p, r + Dp(2), Col(color::text), 24);
                    draw->AddCircleFilled(p, r, tint, 24);
                }
                else draw->AddCircle(p, r - Dp(1), Col(SideTint(shot.side), 0.85f), 24, Dp(2));
                const float d = std::hypot(mouse.x - p.x, mouse.y - p.y);
                if (d < tip_distance && ImGui::IsWindowHovered())
                {
                    static constexpr const char* kKinds[] = {
                        "settings.replays.stats.shots.low", "settings.replays.stats.shots.medium",
                        "settings.replays.stats.shots.high", "settings.replays.stats.shots.skillshot",
                        "settings.replays.stats.shots.mega_strike"};
                    tip_distance = d;
                    tip = ClockText(shot.seconds) + "  " + texts::T(kKinds[int(shot.kind)]) + "  \xC2\xB7  "
                        + PlayerName(report.sides[std::size_t(shot.side)], shot.player)
                        + (shot.scored ? "  \xC2\xB7  " + std::string(texts::T("settings.replays.stats.shots.scored")) : "");
                }
            }
        ImGui::Dummy({width, map_h + Dp(4)});
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        EndCard();
    }

    // Each side's items: the most used, then every kind with how often.
    void ReplayStatsItems(const ReplayStats& stats)
    {
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        if (report.item_counts[0].empty() && report.item_counts[1].empty()) return;
        BeginCard("##stats_items", texts::T("settings.replays.stats.items.title"), nullptr, Icon::Layers);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x, column = (width - Dp(24)) * 0.5f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        float bottom = at.y;
        for (int side = 0; side < 2; ++side)
        {
            const float x = at.x + side * (column + Dp(24));
            float y = at.y;
            const std::string label = Elide(fonts.label, SideLabel(stats, side), column);
            DrawLabel(draw, fonts.label, {x, y}, Col(SideTint(side)), label.c_str());
            y += TextSize(fonts.label, label.c_str()).y + Dp(8);
            const auto& counts = report.item_counts[std::size_t(side)];
            if (counts.empty())
            {
                DrawLabel(draw, fonts.body, {x, y}, Col(color::muted), texts::T("settings.replays.stats.items.none"));
                y += TextSize(fonts.body, "A").y + Dp(6);
            }
            for (std::size_t n = 0; n < counts.size(); ++n)
            {
                const ItemCount& count = counts[n];
                const std::string name = Elide(fonts.body, ItemName(count.type, count.count), column - Dp(90));
                const std::string uses = texts::Count("settings.replays.stats.items.uses", count.uses);
                const ImVec2 uses_size = TextSize(fonts.body, uses.c_str());
                if (n == 0)
                {
                    // The most used, on a tinted band.
                    draw->AddRectFilled({x - Dp(6), y - Dp(4)}, {x + column, y + uses_size.y + Dp(4)},
                                        Col(SideTint(side), 0.12f), Dp(6));
                }
                DrawLabel(draw, fonts.body, {x, y}, Col(n == 0 ? color::text : color::muted), name.c_str());
                DrawLabel(draw, fonts.body, {x + column - uses_size.x - Dp(6), y}, Col(color::muted), uses.c_str());
                y += uses_size.y + Dp(10);
            }
            bottom = std::max(bottom, y);
        }
        ImGui::Dummy({width, bottom - at.y});
        EndCard();
    }

    // Each player's line: goals, assists, shots, passes, hits, steals, items.
    void ReplayStatsPlayers(const ReplayStats& stats)
    {
        static constexpr std::pair<PlayerColumn, const char*> kColumns[] = {
            {PlayerColumn::Goals, "settings.replays.stats.players.goals"},
            {PlayerColumn::Assists, "settings.replays.stats.players.assists"},
            {PlayerColumn::Shots, "settings.replays.stats.players.shots"},
            {PlayerColumn::Passes, "settings.replays.stats.players.passes"},
            {PlayerColumn::Hits, "settings.replays.stats.players.hits"},
            {PlayerColumn::Steals, "settings.replays.stats.players.steals"},
            {PlayerColumn::Items, "settings.replays.stats.players.items"},
        };
        const auto& fonts = CurrentFonts();
        const MatchReport& report = stats.report;
        BeginCard("##stats_players", texts::T("settings.replays.stats.players.title"), nullptr, Icon::Person);
        auto* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const float name_w = std::max(Dp(150), width - Dp(70) * float(std::size(kColumns)));
        const float column_w = (width - name_w) / float(std::size(kColumns));
        const float line = Dp(36), face = Dp(26);
        for (int side = 0; side < 2; ++side)
        {
            const SideReport& team = report.sides[std::size_t(side)];
            if (side == 1) ImGui::Dummy(Dp(0, 10));
            ImVec2 at = ImGui::GetCursorScreenPos();
            const std::string label = Elide(fonts.label, SideLabel(stats, side), name_w - Dp(8));
            DrawLabel(draw, fonts.label, at, Col(SideTint(side)), label.c_str());
            for (std::size_t c = 0; c < std::size(kColumns); ++c)
            {
                const std::string head = Elide(fonts.caption, texts::T(kColumns[c].second), column_w - Dp(4));
                const ImVec2 size = TextSize(fonts.caption, head.c_str());
                DrawLabel(draw, fonts.caption, {at.x + name_w + column_w * (float(c) + 1.0f) - size.x - Dp(4), at.y + Dp(3)},
                          Col(color::dim), head.c_str());
            }
            ImGui::Dummy({width, Dp(24)});
            for (int player = 0; player < 5; ++player)
            {
                at = ImGui::GetCursorScreenPos();
                const float middle = at.y + line * 0.5f;
                if (player % 2 == 0) draw->AddRectFilled(at, {at.x + width, at.y + line}, Col(color::field, 0.5f), Dp(6));
                DrawPlayerFace(draw, {at.x + Dp(6) + face * 0.5f, middle}, face, team, player, SideTint(side));
                const std::string name = Elide(fonts.body, PlayerName(team, player), name_w - face - Dp(24));
                const ImVec2 name_size = TextSize(fonts.body, name.c_str());
                DrawLabel(draw, fonts.body, {at.x + face + Dp(16), middle - name_size.y * 0.5f}, Col(color::text),
                          name.c_str());
                const auto& values = report.players[std::size_t(side)][std::size_t(player)];
                for (std::size_t c = 0; c < std::size(kColumns); ++c)
                {
                    const int value = values[std::size_t(kColumns[c].first)];
                    const std::string number = std::to_string(value);
                    const ImVec2 size = TextSize(fonts.label, number.c_str());
                    DrawLabel(draw, fonts.label,
                              {at.x + name_w + column_w * (float(c) + 1.0f) - size.x - Dp(4), middle - size.y * 0.5f},
                              Col(value ? color::text : color::dim), number.c_str());
                }
                ImGui::Dummy({width, line});
            }
        }
        EndCard();
    }

    // ------------------------------------------------------- texture packs
    fs::path TextureFolder() const { return ResolveTextureFolder(draft_, file_.path).lexically_normal(); }

    // The packs in the player's order (mods.texture_pack_order).
    std::vector<platform::TexturePack> OrderedTexturePacks() const
    {
        return platform::OrderTexturePacks(pack_library_.Packs(), platform::SplitPackNames(draft_.texture_pack_order));
    }

    void MoveTexturePack(std::vector<platform::TexturePack> packs, std::size_t from, std::size_t to)
    {
        if (const auto order = PackOrderAfterMove(std::move(packs), from, to))
        {
            draft_.texture_pack_order = *order;
            dirty_ = true;
        }
    }

    void SetTexturePackOn(const std::string& name, bool on)
    {
        draft_.texture_packs_off = PacksOffWith(draft_.texture_packs_off, name, on);
        dirty_ = true;
    }

    // What became of a pack dropped onto the Extras page (TexturePackLibrary);
    // something that is no pack is a game image or folder, as on other pages.
    void TexturePackEvent(const TexturePackLibrary::Event& event)
    {
        using Kind = TexturePackLibrary::Event::Kind;
        switch (event.kind)
        {
        case Kind::Added:
            Notice(texts::T("settings.gameplay.mods.pack_added", {{"pack", event.pack}}), NoticeKind::Success);
            break;
        case Kind::AddFailed: Notice(event.error, NoticeKind::Error); break;
        case Kind::AlreadyInFolder:
            Notice(texts::T("settings.gameplay.mods.already_in_folder", {{"pack", event.pack}}), NoticeKind::Info);
            break;
        case Kind::FolderHasPack:
            Notice(texts::T("settings.gameplay.mods.folder_has_pack", {{"pack", event.pack}}), NoticeKind::Warning);
            break;
        case Kind::CopyBusy:
            Notice(texts::T("settings.gameplay.mods.wait_for_copy", {{"pack", event.pack}}), NoticeKind::Warning);
            break;
        case Kind::NotAPack: SelectDisc(PathUtf8(event.source)); break;
        }
    }

    void ModsCard()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##mods", texts::T("settings.gameplay.mods.title"), texts::T("settings.gameplay.mods.about"),
                  Icon::Sliders);
        if (BeginRows("##mods_rows"))
        {
            Row(texts::T("settings.gameplay.mods.texture_packs.name"),
                texts::T("settings.gameplay.mods.texture_packs.about"));
            if (Toggle("##texture_packs", &draft_.texture_packs)) dirty_ = true;
            EndRows();
        }
        ImGui::BeginDisabled(!draft_.texture_packs);
        TextureFolderPicker();
        ImGui::Dummy({0, Dp(4)});
        const bool adding = pack_library_.Checking() || pack_library_.Copying();
        if (adding) PackCopyLine();
        const auto packs = OrderedTexturePacks();
        if (!packs.empty())
        {
            TexturePackTable(packs);
            if (packs.size() > 1)
                TextWrappedColored(fonts.caption, color::dim, texts::T("settings.gameplay.mods.order_note"));
        }
        else if (!adding)
            PackDropZone();
        ImGui::EndDisabled();

        ImGui::Dummy({0, Dp(2)});
        TextWrappedColored(fonts.caption, color::muted, texts::T("settings.gameplay.mods.bug_report_note"));
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

        const char* change = texts::T("settings.gameplay.mods.folder.change");
        const char* open = texts::T("settings.gameplay.mods.folder.open");
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
        if (pack_library_.Scanning() && pack_library_.Packs().empty())
        {
            status = texts::T("settings.gameplay.mods.folder.looking");
            tint = color::info;
        }
        else if (!pack_library_.FolderExists())
            status = texts::T("settings.gameplay.mods.folder.missing");
        else
        {
            const auto off = platform::SplitPackNames(draft_.texture_packs_off);
            std::size_t used = 0, textures = 0;
            for (const auto& pack : pack_library_.Packs())
                if (pack.error.empty() && std::find(off.begin(), off.end(), pack.name) == off.end())
                {
                    ++used;
                    textures += pack.textures;
                }
            const auto count = pack_library_.Packs().size();
            status = count == 0 ? std::string(texts::T("settings.gameplay.mods.folder.no_packs"))
                : texts::Count("settings.gameplay.mods.folder.packs", count,
                               {{"used", std::to_string(used)},
                                {"textures", texts::Count("settings.gameplay.mods.textures", textures)}});
        }
        const float text_x = box.x + Dp(36 + 14), text_w = std::max(Dp(80), refresh_x - Dp(14) - text_x);
        const float lines = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
        const float top = center - lines * 0.5f;
        DrawPath(draw, fonts.label, {text_x, top}, text_w, color::text, PathUtf8(path));
        DrawClipped(draw, fonts.caption, {text_x, top + FontSize(fonts.label) + Dp(3)}, text_w, tint, status);
        if (picked) BrowseFolder(FolderPurpose::TextureFolder, path);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", PathUtf8(path).c_str(),
                              texts::T("settings.gameplay.mods.folder.click_tooltip"));

        ImGui::SetCursorScreenPos({refresh_x, center - Dp(17)});
        if (GhostButton("refresh_texture_packs", "", Dp(34, 34), config_ok_ && !pack_library_.Scanning(), Icon::Refresh))
            pack_library_.Rescan();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.gameplay.mods.folder.refresh_tooltip"));
        ImGui::SetCursorScreenPos({open_x, center - Dp(17)});
        if (GhostButton("open_texture_folder", open, {open_w, Dp(34)}, config_ok_, Icon::Folder))
        {
            fs::create_directories(path, error);
            if (error)
                Notice(texts::T("settings.gameplay.mods.cannot_create",
                                {{"path", PathUtf8(path)}, {"error", error.message()}}),
                       NoticeKind::Error);
            else
            {
                OpenPath(path);
                pack_library_.Rescan();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.gameplay.mods.folder.open_tooltip"));
        ImGui::SetCursorScreenPos({change_x, center - Dp(17)});
        if (SecondaryButton("change_texture_folder", change, {change_w, Dp(34)}, can_select))
            BrowseFolder(FolderPurpose::TextureFolder, path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.gameplay.mods.folder.change_tooltip"));
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
        // Checked first (a large folder takes a while), then copied in.
        const std::string text = pack_library_.Checking()
            ? texts::T("settings.gameplay.mods.checking", {{"name", pack_library_.AddingName()}})
            : texts::T("settings.gameplay.mods.adding",
                       {{"pack", pack_library_.AddingName()},
                        {"textures", texts::Count("settings.gameplay.mods.textures", pack_library_.AddingTextures())}});
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
        const char* title = texts::T("settings.gameplay.mods.drop.title");
        const char* detail = texts::T("settings.gameplay.mods.drop.about");
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
            const auto column = [](const char* name, const char* id, ImGuiTableColumnFlags flags, float width) {
                ImGui::TableSetupColumn(StableLabel(name, id).c_str(), flags, width);
            };
            column(texts::T("settings.gameplay.mods.table.order"), "order", fixed, Dp(58));
            column(texts::T("settings.gameplay.mods.table.pack"), "pack", ImGuiTableColumnFlags_WidthStretch, 0.0f);
            column(texts::T("settings.gameplay.mods.table.textures"), "textures", fixed, Dp(84));
            column(texts::T("settings.gameplay.mods.table.in_use"), "in_use", fixed, Dp(64));
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
            ImGui::SetTooltip("%s%s%s", PathUtf8(pack.path).c_str(), reorder ? "\n" : "",
                              reorder ? texts::T("settings.gameplay.mods.table.drag_tooltip") : "");
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
        std::string detail = pack.archive ? texts::T("settings.gameplay.mods.table.zip_file")
                                          : texts::T("settings.gameplay.mods.table.folder");
        ImVec4 detail_tint = color::muted;
        if (!readable)
        {
            detail = texts::T("settings.gameplay.mods.table.unreadable", {{"error", pack.error}});
            detail_tint = color::danger;
        }
        else if (!on)
            detail = pack.archive ? texts::T("settings.gameplay.mods.table.zip_file_off")
                                  : texts::T("settings.gameplay.mods.table.folder_off");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", texts::T("settings.gameplay.mods.table.more_tooltip"));
        const float width = Dp(220);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, Dp(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(10));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(color::card, color::window, 0.25f));
        if (ImGui::BeginPopup("pack_menu"))
        {
            ImGui::BeginDisabled(!reorder || n == 0);
            if (MenuAction("up", texts::T("settings.gameplay.mods.table.move_up"), width))
                MoveTexturePack(packs, n, n - 1);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!reorder || n + 1 >= packs.size());
            if (MenuAction("down", texts::T("settings.gameplay.mods.table.move_down"), width))
                MoveTexturePack(packs, n, n + 1);
            ImGui::EndDisabled();
            MenuSeparator(width);
            if (MenuAction("show", texts::T("settings.gameplay.mods.table.show_in_folder"), width))
                OpenPath(pack.path.parent_path());
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        ImGui::PopID();
    }

    // The launcher itself, its updates and where Recharged keeps its files.
    // The options for testing and development are only in the settings file
    // (docs/SETTINGS.md); the game's diagnostics are its OPTIONS > SETTINGS >
    // ADVANCED.
    void AdvancedPage()
    {
        PageHeader(texts::T("settings.advanced.title"), texts::T("settings.advanced.about"));
        BeginCard("##interface", texts::T("settings.advanced.interface.title"), nullptr, Icon::Sliders);
        if (BeginRows("##interface_rows"))
        {
            Row(texts::T("settings.advanced.interface.menu_language.name"),
                texts::T("settings.advanced.interface.menu_language.about"));
            MenuLanguageControl();
            Row(texts::T("settings.advanced.interface.launcher_size.name"),
                texts::T("settings.advanced.interface.launcher_size.about"));
            const char* preview = draft_.ui_scale.c_str();
            for (const auto& size : kLauncherSizes)
                if (draft_.ui_scale == size.value) preview = texts::ChoiceLabel("launcher_sizes", size);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##ui_scale", preview))
            {
                for (const auto& size : kLauncherSizes)
                    if (ImGui::Selectable(StableLabel(texts::ChoiceLabel("launcher_sizes", size), size.value).c_str(),
                                          draft_.ui_scale == size.value))
                    { draft_.ui_scale = size.value; dirty_ = true; }
                ImGui::EndCombo();
            }
            EndRows();
        }
        EndCard();

        BeginCard("##updates", texts::T("settings.advanced.updates.title"), texts::T("settings.advanced.updates.about"),
                  Icon::Download);
        if (BeginRows("##update_rows"))
        {
            Row(texts::T("settings.advanced.updates.status"), UpdateSummary().c_str());
            ImGui::BeginDisabled(!updates_ || !config_ok_ || UpdateBusy());
            if (SecondaryButton("##check_now", texts::T("settings.advanced.updates.check_now"), Dp(150, 38), true,
                                Icon::Refresh))
                CheckForUpdates(UpdatePrompt::Any, UpdateChannelSetting());
            ImGui::EndDisabled();
            EndRows();
        }
        EndCard();

        BeginCard("##files", texts::T("settings.advanced.files.title"), nullptr, Icon::Folder);
        if (BeginRows("##files_rows"))
        {
            const std::string settings_path = PathUtf8(file_.path);
            Row(texts::T("settings.advanced.files.settings_file"), settings_path.c_str());
            if (SecondaryButton("open_settings", texts::T("settings.advanced.files.open_folder"),
                                {ImGui::GetContentRegionAvail().x, Dp(40)}, fs::exists(file_.path.parent_path()),
                                Icon::Folder))
                OpenPath(file_.path.parent_path());

            const auto logs = base_ / "logs";
            const auto logs_folder = PathUtf8(logs);
            Row(texts::T("settings.advanced.files.logs"), logs_folder.c_str());
            if (SecondaryButton("open_logs", texts::T("settings.advanced.files.open_folder"),
                                {ImGui::GetContentRegionAvail().x, Dp(40)}, true, Icon::Folder))
            {
                std::error_code error;
                fs::create_directories(logs, error);
                OpenPath(logs);
            }
            EndRows();
        }
        EndCard();

        BeginCard("##reset", texts::T("settings.advanced.reset.title"), nullptr, Icon::Refresh);
        if (BeginRows("##reset_rows"))
        {
            Row(texts::T("settings.advanced.reset.defaults.name"), texts::T("settings.advanced.reset.defaults.about"));
            if (SecondaryButton("defaults", texts::T("settings.advanced.reset.defaults.button"),
                                {ImGui::GetContentRegionAvail().x, Dp(40)}, config_ok_, Icon::Refresh))
            {
                const auto disc = draft_.disc;
                const auto folder = draft_.folder;
                const bool subfolders = draft_.folder_subfolders;
                draft_ = {};
                draft_.disc = disc;
                draft_.folder = folder;
                draft_.folder_subfolders = subfolders;
                dirty_ = true;
                Notice(texts::T("settings.advanced.reset.defaults.restored"), NoticeKind::Info);
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
        const float logo = Dp(88);
        const float height = pad + FontSize(fonts.caption) + Dp(6) + logo + Dp(6) + FontSize(fonts.heading) + pad;
        const ImVec2 max{pos.x + width, pos.y + height};
        const float rounding = Dp(16);

        // An accent wash and, on the right, a lightning bolt in a soft glow.
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
        DrawTracked(draw, fonts.caption, {x, y}, Col(color::accent, 0.95f), texts::T("settings.about.overline"),
                    Dp(2.6f));
        y += FontSize(fonts.caption) + Dp(6);
        DrawLogo(draw, {x - Dp(5), y}, logo);
        y += logo + Dp(6);
        const char* game = "Mario Strikers Charged";
        DrawLabel(draw, fonts.heading, {x, y}, Col(color::text), game);
        DrawLabel(draw, fonts.heading, {x + TextSize(fonts.heading, game).x, y}, Col(color::text, 0.72f),
                  texts::T("settings.about.natively"));

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
        BeginCard("##developers", texts::T("settings.about.developers.title"),
                  texts::T("settings.about.developers.about"), Icon::Code);
        PeopleTiles({"Yannick Suter", texts::T("settings.about.developers.yannick"), color::accent_hi},
                    {"GoldenPalazzo", texts::T("settings.about.developers.goldenpalazzo"), color::info});
        EndCard();
    }

    void AboutThanks()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##thanks", texts::T("settings.about.thanks.title"), texts::T("settings.about.thanks.about"),
                  Icon::Heart);

        AboutOverline(texts::T("settings.about.thanks.special.overline"));
        TextWrappedColored(fonts.body, color::text, texts::T("settings.about.thanks.special.about"));
        ImGui::Dummy({0, Dp(4)});
        PeopleTiles({"einBirnenbaum", texts::T("settings.about.thanks.special.tester"), color::accent},
                    {"GoldCobra", texts::T("settings.about.thanks.special.tester"), color::warning});

        AboutDivider();
        AboutOverline(texts::T("settings.about.thanks.reporters.overline"));
        TextWrappedColored(fonts.body, color::text, texts::T("settings.about.thanks.reporters.about"));
        ImGui::Dummy({0, Dp(4)});
        NamePills("##reporters", kIssueReporters, std::size(kIssueReporters), false);
        ImGui::Dummy({0, Dp(6)});
        const BuildIdentity identity = updates_ ? updates_->Build() : CurrentBuild();
        const std::string issues = "https://github.com/" + identity.repository + "/issues/new";
        const ImVec2 button = Dp(200, 40);
        FlowNext(true, button.x);
        if (SecondaryButton("report_problem", texts::T("settings.about.thanks.reporters.report"), button, true,
                            Icon::Bug))
            SDL_OpenURL((issues + "/choose").c_str());
        FlowNext(false, button.x);
        if (SecondaryButton("suggest_feature", texts::T("settings.about.thanks.reporters.suggest"), button, true,
                            Icon::Bulb))
            SDL_OpenURL((issues + "?template=feature_request.yml").c_str());

        AboutDivider();
        AboutOverline(texts::T("settings.about.thanks.decompilation.overline"));
        TextWrappedColored(fonts.body, color::text, texts::T("settings.about.thanks.decompilation.about"));
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
        const char* about = texts::T("settings.about.community.about");
        const char* thanks = texts::T("settings.about.community.thanks");
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
        DrawTracked(draw, fonts.caption, {text_x, y}, Col(Mix(color::discord, color::text, 0.45f)),
                    texts::T("settings.about.community.overline"), Dp(1.8f));
        y += FontSize(fonts.caption) + Dp(4);
        DrawLabel(draw, fonts.heading, {text_x, y}, Col(color::text), texts::T("settings.about.community.title"));
        y += FontSize(fonts.heading) + Dp(8);
        draw->AddText(fonts.body, FontSize(fonts.body), {text_x, y}, Col(color::muted), about, nullptr, wrap);
        y += text_height(about) + Dp(4);
        draw->AddText(fonts.body, FontSize(fonts.body), {text_x, y}, Col(color::text), thanks, nullptr, wrap);
        y += text_height(thanks) + Dp(16);
        ImGui::SetCursorScreenPos({text_x, y});
        if (DiscordButton("join_discord", texts::T("settings.about.community.join"), {Dp(200), button_height}))
            SDL_OpenURL(invite);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", invite);

        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({width, height});
        ImGui::Dummy({0, Dp(8)});
    }

    // What Recharged is built on: the decompilation and Aurora, side by side when they fit.
    void AboutFoundations()
    {
        const auto& fonts = CurrentFonts();
        BeginCard("##foundations", texts::T("settings.about.foundations.title"), nullptr, Icon::Layers);
        const float title = Dp(30);
        auto decomp_heading = [&] {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(pos, {pos.x + title, pos.y + title}, Col(color::accent, 0.12f), Dp(8));
            DrawIcon(draw, Icon::Disc, {pos.x + title * 0.5f, pos.y + title * 0.5f}, Dp(16), Col(color::accent));
            DrawLabel(draw, fonts.label, {pos.x + title + Dp(12), pos.y + (title - FontSize(fonts.label)) * 0.5f},
                      Col(color::text), texts::T("settings.about.foundations.decompilation"));
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
        const char* decomp_text = texts::T("settings.about.foundations.decompilation_about");
        const char* aurora_text = texts::T("settings.about.foundations.aurora_about");
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
            link("decomp_link", texts::T("settings.about.foundations.decompilation_link"), decomp_url);
            ImGui::Dummy({0, Dp(14)});
            aurora_heading();
            text(aurora_text);
            link("aurora_link", texts::T("settings.about.foundations.aurora_link"), aurora_url);
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
                link("decomp_link", texts::T("settings.about.foundations.decompilation_link"), decomp_url);
                ImGui::TableSetColumnIndex(2);
                link("aurora_link", texts::T("settings.about.foundations.aurora_link"), aurora_url);
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }
        AboutDivider();
        TextWrappedColored(fonts.caption, color::muted, texts::T("settings.about.foundations.unofficial"));
        ImGui::Dummy({0, Dp(2)});
        TextWrappedColored(fonts.caption, color::dim,
            "Recharged's original code, tools and documentation: CC0 1.0. Header artwork: the game's key art "
            "(Nintendo). Font: Roboto (Apache 2.0). Built with SDL3 (zlib), Dear ImGui (MIT), Aurora (MIT), Dawn "
            "(BSD), nod and its Rust dependencies, and other libraries listed in LICENSES. Each keeps its own "
            "license.");
        EndCard();
    }

    // Facts about this build in a grid, and what a bug report needs: a copy of
    // them and the log.
    void AboutBuild()
    {
        const auto& fonts = CurrentFonts();
        const BuildIdentity identity = updates_ ? updates_->Build() : CurrentBuild();
        // Each fact on screen in the player's language, and in English in the
        // copy for a bug report, as the log is.
        struct Fact
        {
            const char* label;
            const char* english;
            std::string value;
            std::string english_value;
        };
        std::vector<Fact> facts;
        const auto add = [&](const char* label, const char* english, const std::string& value) {
            facts.push_back({label, english, value, value});
        };
        add(texts::T("settings.about.build.version"), "Version", build::version);
        add(texts::T("settings.about.build.origin"), "Origin", DescribeOrigin(identity));
        if (identity.built) add(texts::T("settings.about.build.built"), "Built", LocalTime(identity.built));
        else if (identity.commit_time)
            add(texts::T("settings.about.build.commit_time"), "Commit time", LocalTime(identity.commit_time));
        add(texts::T("settings.about.build.commit"), "Commit", identity.commit.substr(0, 12));
        const int sdl = SDL_GetVersion();
        add(texts::T("settings.about.build.platform"), "Platform",
            std::string(SDL_GetPlatform()) + ", SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl)) + "."
                + std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(sdl)));
        const char* renderer = SDL_GetRendererName(renderer_);
        char display[96];
        std::snprintf(display, sizeof display, "%s, interface x%.2f, pixel density x%.2f",
                      renderer ? renderer : "unknown renderer", metrics_.ui, metrics_.density);
        add(texts::T("settings.about.build.launcher_display"), "Launcher display", display);
#ifdef MSCHARGED_HAS_ORIGINAL_FRONTEND
        facts.push_back({texts::T("settings.about.build.game_runtime"), "Game runtime",
                         texts::T("settings.about.build.included"), "Included"});
#else
        facts.push_back({texts::T("settings.about.build.game_runtime"), "Game runtime",
                         texts::T("settings.about.build.not_included"), "Not included (launcher-only build)"});
#endif
        add(texts::T("settings.about.build.configuration"), "Configuration", MSCHARGED_BUILD_CONFIG);

        BeginCard("##build", texts::T("settings.about.build.title"), texts::T("settings.about.build.about"),
                  Icon::Info);
        const int columns = ImGui::GetContentRegionAvail().x >= Dp(600) ? 3 : 2;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 7));
        if (ImGui::BeginTable("##build_facts", columns, ImGuiTableFlags_SizingStretchSame))
        {
            for (const auto& fact : facts)
            {
                ImGui::TableNextColumn();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - Dp(16));
                TextColored(fonts.caption, color::dim, fact.label);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(4));
                TextWrappedColored(fonts.body, color::text, fact.value.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::Dummy({0, Dp(4)});

        const ImVec2 button = Dp(200, 40);
        FlowNext(true, button.x);
        if (SecondaryButton("copy_build", texts::T("settings.about.build.copy"), button, true, Icon::Copy))
        {
            std::string text = "Recharged\n";
            for (const auto& fact : facts) text += std::string(fact.english) + ": " + fact.english_value + "\n";
            if (SDL_SetClipboardText(text.c_str()))
                Notice(texts::T("settings.about.build.copied"), NoticeKind::Success);
        }
        FlowNext(false, button.x);
        if (SecondaryButton("open_logs", texts::T("settings.about.build.open_logs"), button, true, Icon::Folder))
        {
            const fs::path log = platform::SessionLogPath();
            const fs::path folder = log.empty() ? base_ / "logs" : log.parent_path();
            std::error_code error;
            fs::create_directories(folder, error);
            OpenPath(folder);
        }
        FlowNext(false, button.x);
        const char* notes = identity.origin == BuildOrigin::Test      ? texts::T("settings.about.build.pull_request")
                          : identity.origin == BuildOrigin::Release ? texts::T("settings.about.build.release_notes")
                                                                    : texts::T("settings.about.build.all_releases");
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
    SDL_Texture* header_back_ = nullptr;
    SDL_Texture* header_front_ = nullptr;
    SDL_Texture* header_strokes_ = nullptr;
    // The Recharged logo, largest copy first, each half the one before.
    struct LogoLevel
    {
        SDL_Texture* texture;
        float width, height;
    };
    std::vector<LogoLevel> logo_;
    std::vector<LogoLevel> logo_sizzle_; // the logo with sparks, the same sizes
    float logo_aspect_ = 1.0f;
    struct SizzleState
    {
        std::mt19937 rng{std::random_device{}()};
        double start = 0, length = 0, next = 1.0, flicker_until = 0;
        float flicker = 0;
    } sizzle_;
    SDL_Texture* aurora_logo_ = nullptr;
    float aurora_logo_width_ = 1, aurora_logo_height_ = 1;
    SDL_Texture* flags_ = nullptr; // assets/flags/flags.png
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
    InputCapture input_capture_;
    bool view_opened_clean_ = false; // no unsaved changes when the keyboard or gamepad view opened
    // Gamepad profiles view: the profile being edited, its rename field and
    // the input waiting for a gamepad press.
    bool gamepad_view_ = false;
    int edit_profile_ = -1; // below 0: built in (-1 Default)
    std::size_t pad_slot_ = 0; // the gamepad the page was opened for
    bool renaming_ = false, rename_focus_ = false, confirm_delete_ = false;
    std::string rename_buffer_;
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
    float hero_time_ = 0; // seconds into the header's entrance
    bool hero_shown_ = false; // drawn in the frame before
    bool window_ui_ready_ = false;
    bool renderer_ui_ready_ = false;
    ConfigFile file_;
    Settings draft_;
    bool config_ok_ = false;
    bool dirty_ = false;
    bool play_prompt_ = false;
    float profile_actions_ = 0.0f; // Player profile: where the rows' actions start, from the value
    // Start training: asked on the Extras page, and the start it makes.
    bool training_requested_ = false;
    // A set's go seen: start its game (once per set and launcher).
    bool competitive_go_ = false;
    std::int64_t competitive_started_go_ = 0; // GoKey of the set's go this launcher started the game for
    bool training_ = false;
    NoticeKind notice_kind_ = NoticeKind::Info;
    std::string notice_;
    int page_ = PagePlay;
    int group_page_[std::size(kNavGroups)] = {PageOnlineSettings, PageGraphics}; // the page each group's row opens
    bool close_requested_ = false;
    bool region_prompt_ = false;
    DiscCheck check_;
    // The PC's languages, for Automatic (read once).
    std::vector<std::string> pc_languages_ = mscharged::platform::PcLanguages();
    // Recharged's own texts: the menu language they are in (LoadTexts), and
    // the languages assets/lang has.
    std::optional<std::string> texts_language_;
    std::vector<texts::LanguageChoice> menu_languages_;
    BackgroundJob<DiscCheck> disc_check_; // the chosen image's check
    // An image chosen with Browse, a drop or Locate, checked before it is pinned.
    BackgroundJob<DiscCheck> candidate_;
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
    BackgroundJob<LibraryScan> library_scan_;
    std::shared_ptr<ScanProgress> library_progress_;
    bool library_rescan_ = false;
    // Who chooses an image when the next scan is done, if nothing is chosen then.
    enum class AutoChoice { None, FirstStart, NewFolder };
    AutoChoice autochoose_ = AutoChoice::None;
    LibrarySort library_sort_ = LibrarySort::Release;
    bool library_descending_ = false;
    std::map<std::string, SDL_Texture*> banners_; // by disc ID
    std::set<std::string> banner_failures_, banner_missing_;
    // The background search for a disc image when none is chosen.
    BackgroundJob<launcher::DiscSearchResult> search_;
    // The PC's graphics cards (Advanced -> Graphics card), listed once at start.
    BackgroundJob<std::vector<platform::GraphicsCard>> graphics_cards_scan_;
    std::vector<platform::GraphicsCard> graphics_cards_;
    std::string search_from_, search_folder_from_; // the disc and game folder settings when the search began
    std::vector<std::string> displays_;
    std::vector<SDL_DisplayID> display_ids_;
    std::shared_ptr<DialogResult> dialog_ = std::make_shared<DialogResult>();
    std::shared_ptr<DialogResult> folder_dialog_ = std::make_shared<DialogResult>();
    FolderPurpose folder_purpose_ = FolderPurpose::SaveFolder;
    // The texture packs folder's packs (Gameplay -> Mods), read in the background.
    TexturePackLibrary pack_library_; // the Extras page's texture packs
    fs::path import_source_;
    ImportKind import_kind_ = ImportKind::Save;
    bool import_prompt_ = false;
    // The Miis' heads (MiiFaces), and whether the game image's parts failed
    // when last seen (logged once).
    std::unique_ptr<MiiFaces> mii_faces_;
    bool mii_faces_failed_ = false;
    // The Miis of the save folder in use and the times of their files when read.
    fs::path miis_folder_;
    std::vector<fs::file_time_type> miis_times_;
    Uint64 miis_checked_ = 0; // SDL ticks of the last look at the Mii files' times
    mscharged::platform::MiiDatabaseStatus miis_;
    // The Online player profile page's Create (signed out) or Finish (a
    // profile a Discord sign-in made): the name, the new Mii's colour and face
    // (rolled from `seed`, or edited; its create ID counts from `created`
    // until it is made), the Mii of this computer taken instead (the new one
    // once made), and the Create or Save on its way (`serial`: the profile's
    // changes counted before Save). `typed_at`: when the name was last typed
    // (SDL ticks), `asked_name` the name last asked about.
    struct ProfileMaker
    {
        bool open = false;
        bool finish = false;
        std::string name;
        int color = 0;
        std::uint32_t seed = 0;
        platform::mii::MiiFace face;
        std::int64_t created = 0;
        std::optional<std::array<unsigned char, 8>> chosen;
        bool creating = false, saving = false, tried = false;
        std::uint64_t serial = 0;
        Uint64 typed_at = 0;
        std::string asked_name;
    };
    ProfileMaker maker_;
    // The Mii editor (MiiEditorPage): whether it is open and on which page,
    // what it edits (Play with a Mii's new Mii, or one of this computer's:
    // `add` for the player's own Mii not made here yet, `name_locked` for a
    // Discord player's, named after their Discord name), the Mii as opened,
    // the face and name being edited, the steps Undo takes back (a step per
    // value changed, a slider's drag one step), the section and expression
    // shown, and the head shown until the edited one is drawn.
    struct MiiEditor
    {
        enum class Target { Maker, Database };
        struct Step
        {
            platform::mii::MiiFace face;
            std::string name;
        };
        bool open = false;
        int page = 0;
        Target target = Target::Maker;
        // The Mii database the edited Mii came from (Target::Database): Save
        // writes it there, also when another image or save folder was chosen
        // meanwhile.
        fs::path folder;
        std::optional<mscharged::platform::SaveTitle> title;
        std::string record;
        bool add = false, name_locked = false;
        platform::mii::MiiFace face, original_face;
        std::string name, original_name;
        std::vector<Step> undo;
        std::string gesture;
        bool released = true;
        int section = 0, section_shown = -1;
        int expression = 0;
        platform::mii::MiiFace shown;
        int shown_expression = 0;
        bool has_shown = false;
    };
    MiiEditor mii_editor_;
    unsigned mii_editor_frames_ = 0; // frames the editor was drawn in (the smoke test tells)
    bool page_scroll_top_ = false; // the page shown from its top next frame
    bool sign_out_prompt_ = false;
    // The sign-ins and restore answers already acted on (UpdateSignIns,
    // UpdateRestore), and the Mii whose Continue signs in.
    std::uint64_t seen_sign_ins_ = 0, seen_restore_ = 0;
    std::optional<std::array<unsigned char, 8>> continuing_;
    // Renaming the profile: the row's edit (open, its text), and the name
    // asked for while it is on its way (`rename_serial_`: the profile's
    // changes counted before).
    bool profile_renaming_ = false;
    std::string rename_text_, renaming_to_;
    std::uint64_t rename_serial_ = 0;
    // Import (DrawProfileImport, then for a signed-in player
    // DrawImportedProfiles: the imported Miis to take a profile from, and the
    // Mii whose profile goes into this one).
    bool profile_import_prompt_ = false, imported_prompt_ = false, disconnect_prompt_ = false;
    std::vector<platform::InstalledMii> imported_miis_;
    std::optional<std::array<unsigned char, 8>> taking_over_;
    std::uint64_t take_over_serial_ = 0;
    std::string discord_choice_seen_; // the Discord choice (its takeover) asked already
    bool show_all_back_ = false;      // Welcome back lists all its profiles
    bool signing_out_ = false; // Sign out pressed, the service not signed out yet
    // The profiles that signed in here (platform::KnownProfile), read once;
    // the status serial they were last brought up to date at.
    std::vector<platform::KnownProfile> known_profiles_;
    bool known_profiles_read_ = false;
    std::uint64_t known_profiles_serial_ = 0;
    bool preview_applied_ = false; // a development capture's made-up Mii state, set once
    // The Mii made from a Discord account (PlayerOwnMii), for its key and name.
    std::string keyed_mii_for_, keyed_mii_record_;
    // The Teams page: the captain shown, the slot whose menu is open, and the
    // lineups read from the save folder.
    int lineup_captain_ = 0;
    int lineup_slot_ = -1;
    fs::path lineups_folder_;
    fs::file_time_type lineups_time_;
    mscharged::platform::SidekickSaveStatus lineups_;
    // The save folder whose game save the Teams or Matches page backed up this session.
    fs::path save_backed_up_;
    // The Matches page: the presets and their folder, the match settings read
    // from the save folder, the preset being edited and the one whose Delete
    // asks for a second click.
    fs::path presets_folder_;
    fs::file_time_type presets_time_;
    launcher::MatchPresetList presets_;
    bool presets_loaded_ = false;
    std::optional<launcher::MatchPreset> preset_edit_;
    int preset_delete_ = -1;
    // The WFC server's count of players online (top bar of the Play page).
    std::unique_ptr<PlayersOnline> players_online_;
    std::optional<PlayersOnline::Count> online_count_;
    // The server's message for the Play page, and its Markdown's blocks.
    std::unique_ptr<ServerMessageFetch> server_message_fetch_;
    std::optional<ServerMessage> server_message_;
    std::vector<NoteBlock> server_message_blocks_;
    float message_height_ = 0.0f; // the message's height last frame, for the Play page's layout
    // Update check and installation (#50).
    std::unique_ptr<UpdateService> updates_;
    // Competitive play: the service, what it showed last, the sign-in page
    // opened last and the last failure told.
    std::unique_ptr<CompetitiveService> competitive_;
    CompetitiveService::Status competitive_status_;
    std::uint64_t competitive_opened_ = 0, competitive_errors_ = 0;
    // The Discord friends (after the service: it goes first), what they
    // showed last, the friends whose players were asked for and the last
    // failure told.
    std::unique_ptr<DiscordFriends> discord_friends_;
    std::int64_t watched_match_ = 0;    // the open set, told when it ends without a result
    bool watched_invite_ = false;       // it was still a challenge when last seen
    std::int64_t alerted_ = 0;          // the last chime: a challenge's id * 2, or a go's GoKey * 2 + 1
    std::int64_t countdown_go_ = 0;     // the go counting down (GoKey), and when the game starts (SDL ticks)
    std::uint64_t countdown_end_ = 0;
    std::string set_blocker_;           // why this computer can't play a set now (SetBlocker), or empty
    // The challenge dialog: the challenge declined (it does not come back),
    // the one accepted here and the last failure before its Accept, and why
    // the set's game could not start.
    std::int64_t challenge_declined_ = 0, challenge_accepted_ = 0;
    std::uint64_t challenge_errors_ = 0;
    std::string set_start_error_;
    std::optional<platform::AlertSound> alert_sound_;
    std::optional<platform::DiscordPresence> set_presence_; // the Discord status of a waiting set
    DiscordFriends::Status discord_status_;
    // The Discord friends' ids the service looks up (the game's online panel
    // gets them too).
    std::vector<std::string> discord_asked_;
    std::uint64_t discord_errors_ = 0;
    std::string test_player_name_;
    UpdateService::Status update_;
    bool update_dialog_ = false, update_capture_dialog_ = false, update_capture_install_ = false, restart_ = false;
    bool update_preview_ = false;
    // Enforced updates (launcher/online_gate.h): the installation the launcher
    // started by itself, and the box that says when one did not work out.
    bool enforced_install_ = false, update_problem_open_ = false, update_problem_online_ = false;
    UpdateProblem update_problem_ = UpdateProblem::None;
    std::string update_problem_reason_;
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
    // The welcome guide (assets/launcher/welcome.md), and whether it is to open.
    std::string welcome_title_;
    std::vector<NoteBlock> welcome_;
    bool welcome_pending_ = false;
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
                         "--non-interactive makes that run a startup diagnostic: it ends once its first frame is captured,\n"
                         "or fails after 90 s.\n"
                         "Use --launcher to open settings; launcher checks and explicit runtime modes keep their selected mode.\n"
                         "Development checks: --smoke-test | --screenshot FILE [--page play|online-settings|rules|sidekicks|game|"
                         "graphics|controls|advanced|extras|replays|about]\n";
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
        else if (arg == "--native-send" || arg == "--diagnostic-frame" || arg == "--resize-check"
                 || arg == "--non-interactive") options.frame_arguments = true;
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
                const auto old = std::find_if(std::begin(kOldPageIds), std::end(kOldPageIds),
                                              [&](const auto& page) { return value == page.first; });
                if (it == std::end(page_ids) && old == std::end(kOldPageIds))
                { std::cerr << "Unknown launcher page: " << value << '\n'; return 2; }
                options.page = it != std::end(page_ids) ? int(it - std::begin(page_ids)) : old->second;
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
    // --disc, or --training, starts the game without the launcher. A replay
    // without --disc opens the launcher, which finds the image it was
    // recorded with and starts it.
    if (!runtime_modes && (options.launch.disc || options.launch.training) && !launcher_requested)
    {
        options.experimental_frontend = true;
        options.direct_game = true;
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
                nullptr, options.direct_game, options.experimental_options ?
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
        bool reopen_on = false;
        // A replay on the command line (opening a .msc file): the launcher is
        // not opened again after it, as it would get that replay once more.
        const bool replay_argument = options.launch.replay.has_value();
        {
            Launcher launcher;
            result = launcher.Run(std::move(options));
            frontend_launch = launcher.FrontendLaunch();
            if (launcher.RestartRequested()) restart = launcher.RestartExecutable();
            // A set's game always opens the launcher again, into its lobby;
            // otherwise as [launcher] reopen_after_game says (the game's
            // Settings can switch it while it runs).
            else if (frontend_launch)
            {
                reopen = launcher.RestartExecutable();
                reopen_on = (frontend_launch->settings.reopen_after_game || frontend_launch->start_competitive)
                    && !replay_argument;
            }
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
        if (!reopen.empty()) mscharged::platform::ArmLauncherReopen(reopen, argc, argv, reopen_on);
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
