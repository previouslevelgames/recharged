#include "launcher/online_panel_preview.h"

#include "launcher/match_presets.h"
#include "platform/competitive_rules.h"

#include "launcher/ui_kit.h"
#include "overlay/overlay_unit.h"
#include "platform/http_client.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace mscharged::launcher
{
namespace
{
using platform::MiiProfile;
using platform::ProfileMatch;
using platform::ProfilePlayer;

ImTextureID TextureId(SDL_Texture* texture)
{
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
}

// A made-up avatar: a figure (head and shoulders) on a two-colour gradient.
SDL_Texture* MakeAvatar(SDL_Renderer* renderer, SDL_Color top, SDL_Color bottom, SDL_Color shirt, SDL_Color hair)
{
    constexpr int size = 128;
    SDL_Surface* surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    const auto mix = [](Uint8 a, Uint8 b, float t) { return Uint8(float(a) + (float(b) - float(a)) * t); };
    for (int y = 0; y < size; ++y)
    {
        auto* row = static_cast<Uint8*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < size; ++x)
        {
            const float t = float(x + y) / float(2 * size);
            SDL_Color c{mix(top.r, bottom.r, t), mix(top.g, bottom.g, t), mix(top.b, bottom.b, t), 255};
            const float hx = float(x) - 64.0f, hy = float(y) - 52.0f;
            const float head = std::sqrt(hx * hx + hy * hy);
            const float bx = float(x) - 64.0f, by = float(y) - 132.0f;
            const bool body = bx * bx / (50.0f * 50.0f) + by * by / (46.0f * 46.0f) < 1.0f;
            if (body) c = shirt;
            if (head < 24.0f) c = hy < -6.0f && head > 10.0f ? hair : SDL_Color{246, 206, 170, 255};
            if (head < 24.0f && hy < -12.0f) c = hair;
            row[x * 4 + 0] = c.r;
            row[x * 4 + 1] = c.g;
            row[x * 4 + 2] = c.b;
            row[x * 4 + 3] = 255;
        }
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

ProfilePlayer Player(std::int64_t id, const char* name, const char* discord)
{
    ProfilePlayer player;
    player.id = id;
    player.name = name;
    player.discord = discord;
    return player;
}

ProfilePlayer Online(ProfilePlayer player, const char* activity)
{
    player.online = true;
    player.activity = activity;
    return player;
}

MiiProfile Mii(std::int32_t id, const char* name, const char* code, bool online, const char* status,
               std::int64_t last_online, bool is_friend)
{
    MiiProfile mii;
    mii.profile_id = id;
    mii.mii = name;
    mii.game = "R4QE";
    mii.friend_code = code;
    mii.online = online;
    mii.status = status;
    mii.last_online = last_online;
    mii.is_friend = is_friend;
    return mii;
}

ProfileMatch Set(const ProfilePlayer& opponent, const char* state, const char* side, const char* stage)
{
    ProfileMatch set;
    set.id = 77;
    set.state = state;
    set.side = side;
    set.stage = stage;
    set.opponent = opponent;
    set.opponent_online = opponent.online;
    set.rule_set = "Competitive";
    set.rules.skill = "rookie";
    set.rules.limit = "time";
    set.rules.time = 180;
    set.rules.games = 3;
    set.rules.stadium = "random";
    set.rules.tweaks = {"fast_terrain", "no_mega_strikes"};
    return set;
}
// A tab to come, for "preview-tabs": its page is a placeholder.
overlay::GameOverlayMoreTab MoreTab(int id, const char* label, Icon icon, int badge = 0, bool dot = false)
{
    overlay::GameOverlayMoreTab more;
    more.tab.id = id;
    more.tab.label = label;
    more.tab.icon = icon;
    more.tab.badge = badge;
    more.tab.dot = dot;
    more.draw = [label = std::string(label), icon](const overlay::OverlayLayout& layout) {
        auto* draw = ImGui::GetWindowDrawList();
        const Fonts& fonts = CurrentFonts();
        const ImVec2 center{(layout.min.x + layout.max.x) * 0.5f, (layout.min.y + layout.max.y) * 0.5f - Dp(20)};
        draw->AddCircleFilled(center, Dp(44), Col(color::text, 0.05f));
        draw->AddCircle(center, Dp(44), Col(color::border), 0, Dp(1));
        DrawIcon(draw, icon, center, Dp(40), Col(color::muted));
        const ImVec2 title = TextSize(fonts.heading, label.c_str());
        DrawLabel(draw, fonts.heading, {center.x - title.x * 0.5f, center.y + Dp(62)}, Col(color::text),
                  label.c_str());
        const char* note = "A tab to come: an empty page, in the launcher's preview only.";
        const ImVec2 size = TextSize(fonts.caption, note);
        DrawLabel(draw, fonts.caption, {center.x - size.x * 0.5f, center.y + Dp(94)}, Col(color::dim), note);
    };
    return more;
}

// RECHARGED_CAPTURE_AVATARS: one of Discord's six default avatars, for a
// player without a made-up picture.
std::string DefaultAvatar(std::int64_t id)
{
    return "https://cdn.discordapp.com/embed/avatars/" + std::to_string(id % 6) + ".png";
}

// The overlay's picture cache as the game has it, its textures made by the
// launcher's renderer; "discord" downloads, any other value is a PNG file
// read for every picture (offline captures).
std::unique_ptr<overlay::OverlayImageCache> PictureCache(SDL_Renderer* renderer, const std::string& source)
{
    if (!renderer || source.empty()) return nullptr;
    auto upload = [renderer](int width, int height, const void* rgba) {
        SDL_Texture* texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
        if (!texture) return ImTextureID(0);
        SDL_UpdateTexture(texture, nullptr, rgba, width * 4);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        return TextureId(texture);
    };
    overlay::OverlayImageCache::Fetch fetch;
    if (source != "discord")
        fetch = [source](const std::string&, std::size_t max_bytes, const std::shared_ptr<platform::HttpCancel>&) {
            std::ifstream file(source, std::ios::binary);
            if (!file) throw std::runtime_error("cannot read " + source);
            std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            if (bytes.size() > max_bytes) throw std::runtime_error("too large");
            return bytes;
        };
    return std::make_unique<overlay::OverlayImageCache>(upload, overlay::OverlayImageLimits{}, fetch);
}

// A label as RECHARGED_CAPTURE_TAB names it: in small letters, a dash for a space.
std::string Slug(std::string text)
{
    for (auto& c : text) c = c == ' ' ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
} // namespace

OnlinePanelPreview::OnlinePanelPreview(SDL_Renderer* renderer, bool capture) : capture_(capture)
{
    const char* mode = std::getenv("RECHARGED_CAPTURE_DISCORD");
    mode_ = mode && std::string(mode).rfind("preview", 0) == 0 ? mode : "preview";
    const auto now = std::int64_t(std::time(nullptr));
    // The window of a 1280x720 game (a small one for "preview-narrow"), at
    // the launcher's scale.
    // RECHARGED_CAPTURE_SIZE=WxH: a game window of that size.
    int game_width = 0, game_height = 0;
    if (const char* size = std::getenv("RECHARGED_CAPTURE_SIZE"))
        if (std::sscanf(size, "%dx%d", &game_width, &game_height) != 2 || game_width < 320 || game_height < 240)
            game_width = game_height = 0;
    if (SDL_Window* window = renderer ? SDL_GetRenderWindow(renderer) : nullptr)
    {
        const bool narrow = mode_.size() >= 7 && mode_.compare(mode_.size() - 7, 7, "-narrow") == 0;
        if (game_height > 0)
        {
            // Smaller than the launcher lets its window be, as a game's may be.
            SDL_SetWindowMinimumSize(window, 0, 0);
            SDL_SetWindowSize(window, game_width, game_height);
            game_width_ = float(game_width);
            game_height_ = float(game_height);
        }
        else
            SDL_SetWindowSize(window, int(std::lround((narrow ? 860.0f : 1280.0f) * UiScale())),
                              int(std::lround((narrow ? 640.0f : 720.0f) * UiScale())));
    }
    struct Look
    {
        const char* key;
        SDL_Color top, bottom, shirt, hair;
    };
    static const Look looks[] = {
        {"preview:me", {58, 176, 255, 255}, {76, 240, 122, 255}, {20, 40, 90, 255}, {60, 36, 20, 255}},
        {"preview:mario", {230, 70, 60, 255}, {255, 180, 67, 255}, {200, 30, 40, 255}, {70, 40, 20, 255}},
        {"preview:peach", {255, 150, 190, 255}, {255, 220, 120, 255}, {240, 110, 170, 255}, {250, 210, 90, 255}},
        {"preview:wario", {240, 210, 40, 255}, {150, 70, 200, 255}, {230, 190, 20, 255}, {50, 30, 20, 255}},
    };
    for (const Look& look : looks)
    {
        SDL_Texture* texture = renderer ? MakeAvatar(renderer, look.top, look.bottom, look.shirt, look.hair) : nullptr;
        if (texture) avatars_[look.key] = TextureId(texture);
    }

    // Pictures as the game draws them (RECHARGED_CAPTURE_AVATARS).
    if (const char* source = std::getenv("RECHARGED_CAPTURE_AVATARS")) pictures_ = PictureCache(renderer, source);
    const auto picture = [this](ProfilePlayer player) {
        if (pictures_ && player.avatar_url.empty()) player.avatar_url = DefaultAvatar(player.id);
        return player;
    };

    ProfilePlayer& me = context_.me;
    me = Player(900000, "Anosch", "anosch");
    me.avatar_url = "preview:me";
    me.available = mode_ != "preview-empty";
    // The server's rule sets, the player's challenges by the first (the second for "preview-rules").
    const auto rule_set = [](std::int64_t id, const char* name, const char* skill, const char* limit, int amount,
                             int games, const char* stadium, std::vector<std::string> tweaks) {
        platform::RuleSet set;
        set.id = id;
        set.name = name;
        set.rules.skill = skill;
        set.rules.limit = limit;
        (set.rules.limit == "time" ? set.rules.time : set.rules.goals) = amount;
        set.rules.games = games;
        set.rules.stadium = stadium;
        set.rules.tweaks = std::move(tweaks);
        return set;
    };
    context_.rule_sets = {
        rule_set(1, "Competitive", "rookie", "time", 180, 3, "random", {"fast_terrain", "no_mega_strikes"}),
        rule_set(2, "Classic", "professional", "time", 300, 1, "random", {}),
        rule_set(3, "First to 5", "legend", "goals", 5, 5, "random", {"thunder_island_walls"})};
    context_.chosen_rules = mode_.rfind("preview-rules", 0) == 0 ? "Classic" : "Competitive";
    // One of the player's own presets: Palace with Field Tilt.
    overlay::OnlinePanelPreset palace{"Palace tilt", context_.rule_sets[0].rules, 0};
    palace.rules.stadium = "the_palace";
    palace.rules.stadium_cheat = "field_tilt";
    context_.my_presets = {palace};
    for (const auto& preset : launcher::BuiltInMatchPresets())
        context_.built_in_presets.push_back({preset.name, launcher::ChallengeRules(preset), 0});
    // Searches for a random opponent: possible; "preview-searching" shows one
    // running (on the search's setup with "preview-search-running").
    context_.searches = true;
    if (mode_ == "preview-searching" || mode_ == "preview-search-running")
    {
        platform::ProfileSearch search;
        search.rule_set = "Palace tilt";
        search.rules = palace.rules;
        search.asked = 3;
        context_.search = search;
        context_.search_seconds = 42;
    }

    auto mario = Online(Player(900010, "Mario", "itsamario"), "launcher");
    mario.avatar_url = "preview:mario";
    mario.is_friend = true;
    auto daisy = Online(Player(900001, "Daisy", "daisy"), "game");
    daisy.is_friend = true;
    daisy.discord_id = "900000000000000001";
    auto luigi = Online(Player(900003, "Luigi", "luigi"), "game");
    luigi.busy = true;
    luigi.is_friend = true;
    auto rosalina = Player(900011, "Rosalina", "rosalina");
    rosalina.is_friend = true;
    rosalina.last_online = now - 2 * 86400;
    auto wario = Player(900004, "Wario", "wario");
    wario.avatar_url = "preview:wario";
    wario.is_friend = true;
    wario.last_online = now - 40 * 60;
    auto peach = Online(Player(900002, "Peach", "peach"), "match");
    peach.avatar_url = "preview:peach";
    peach.discord_id = "900000000000000002";
    auto bowser = Player(900006, "Bowser", "bowser");
    bowser.discord_id = "900000000000000006";
    bowser.last_online = now - 3 * 3600;
    auto shy = Online(Player(900030, "Shy Guy", "shyguy"), "launcher");
    auto koopa = Online(Player(900020, "Koopa", "koopa"), "game");
    for (ProfilePlayer* player : {&daisy, &luigi, &rosalina, &bowser, &shy, &koopa}) *player = picture(*player);

    if (mode_ != "preview-empty")
    {
        lists_.friends = {mario, daisy, luigi, wario, rosalina};
        context_.discord_friends = {daisy, peach, bowser};
        lists_.available = {shy, koopa, mario};
        lists_.game_online = {Mii(1000000011, "Kenji", "3021-8817-4410", true, "match", now, false),
                              Mii(1000000012, "Lou", "1186-0042-7731", true, "menu", now, false)};
        lists_.mii_friends = {Mii(1000000004, "Tommy", "4123-0987-1288", true, "matchmaking", now, true),
                              Mii(1000000007, "Sara", "0868-9934-5920", false, "", now - 5 * 3600, true)};
    }
    if (mode_ == "preview" || mode_ == "preview-narrow")
    {
        // The last set, won 2-1.
        ProfileMatch set = Set(koopa, "finished", "challenger", "finished");
        set.winner = "you";
        set.games = {{3, 1, "mario", "bowser", "the_palace"},
                     {0, 2, "mario", "bowser", "the_dump"},
                     {2, 1, "mario", "bowser", "crater_field"}};
        match_ = set;
    }
    else if (mode_ == "preview-set")
    {
        match_ = Set(mario, "accepted", "challenger", "playing");
        match_->you_ready = match_->opponent_ready = true;
        context_.score = overlay::OnlinePanelContext::Score{1, 0};
    }
    else if (mode_ == "preview-invite" || mode_.rfind("preview-tabs", 0) == 0)
        match_ = Set(peach, "challenged", "challenged", "invited");
    else if (mode_ == "preview-waiting") match_ = Set(shy, "challenged", "challenger", "invited");
    else if (mode_ == "preview-ready")
    {
        match_ = Set(daisy, "accepted", "challenged", "open");
        match_->opponent_ready = true;
    }
    else if (mode_ == "preview-ended")
    {
        ProfileMatch set = Set(mario, "finished", "challenged", "finished");
        set.winner = "opponent";
        set.games = {{1, 2, "wario", "mario", "the_palace"}, {0, 1, "wario", "mario", "vice"}};
        match_ = set;
    }
    else if (mode_ == "preview-match" || mode_ == "preview-options-match")
    {
        context_.in_match = true;
        options_.in_match = true;
    }
    else if (mode_ == "preview-error")
    {
        context_.message = "The server didn't answer. Trying again...";
        context_.error = true;
    }
    else if (mode_ == "preview-signed-out")
    {
        // No player profile: what the game has without a session.
        context_ = {};
        context_.signed_out = true;
        lists_ = {};
    }
    // Some options on, as a player might have them.
    for (const GameTweak tweak : {GameTweak::FastTerrain, GameTweak::NoMegaStrikes, GameTweak::FastMenus})
        options_.tweaks[GameTweakIndex(tweak)] = true;
    // Tabs to come, with made-up badges (ids from 100: never the game's).
    if (mode_.rfind("preview-tabs", 0) == 0)
    {
        more_tabs_ = {MoreTab(100, "Clubs", Icon::Person, 0, true), MoreTab(101, "Tournaments", Icon::Bracket, 2),
                      MoreTab(102, "Stats", Icon::Target)};
        if (mode_.rfind("preview-tabs-max", 0) == 0)
        {
            more_tabs_.push_back(MoreTab(103, "Replays", Icon::Play));
            more_tabs_.back().tab.available = false;
            more_tabs_.back().tab.hint = "Coming later";
            more_tabs_.push_back(MoreTab(104, "Teams", Icon::Person));
            more_tabs_.back().tab.hidden = true;
        }
    }
    {
        overlay::GameOverlayModel tabs;
        tabs.more_tabs = more_tabs_;
        state_.tabs = overlay::GameOverlayTabs(tabs);
    }
    overlay::OverlayTab first = mode_.rfind("preview-options", 0) == 0 ? overlay::OverlayTab::GameOptions
        : mode_.rfind("preview-settings", 0) == 0                     ? overlay::OverlayTab::Settings
        : mode_.rfind("preview-ranked", 0) == 0                       ? overlay::OverlayTab::Ranked
                                                                      : overlay::OverlayTab::Online;
    // The Ranked tab: Play RANKED with the player's own Mii, the leaderboards
    // with the player on both; "preview-ranked-going" on the way there,
    // "preview-ranked-out" without a player profile.
    ranked_.own_mii = true;
    ranked_.can_play = mode_ != "preview-ranked-going";
    ranked_.going = mode_ == "preview-ranked-going";
    ranked_.signed_out = mode_ == "preview-ranked-out";
    if (!ranked_.signed_out)
    {
        const auto entry = [](int rank, const char* mii, const char* player, int wins, int losses, int points,
                              int region, bool you = false) {
            platform::LeaderboardEntry row;
            row.rank = rank;
            row.mii = mii;
            row.player = player;
            row.wins = wins;
            row.losses = losses;
            row.points = points;
            row.region = region;
            row.you = you;
            return row;
        };
        platform::Leaderboard boards;
        boards.season.first_day = "2026-11-01";
        boards.season.last_day = "2026-11-30";
        boards.season.players = 48;
        boards.season.top = {entry(1, "Raulaso", "", 31, 6, 1840, 4),
                             entry(2, "\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA", "", 27, 9, 1655, 1),
                             entry(3, "Birnen\xE2\x84\xA2", "einBirnenbaum", 22, 8, 1490, 4),
                             entry(4, "\xC5\x81ukasz", "", 19, 11, 1302, 4),
                             entry(5, "Anosch", "Anosch", 17, 10, 1215, 4, true),
                             entry(6, "Tommy", "", 15, 12, 1090, 2),
                             entry(7, "\xE2\x98\x85Sara\xE2\x99\xAA", "", 12, 12, 940, 4),
                             entry(8, "\xEC\xB2\xA0\xEC\x88\x98", "", 9, 14, 760, 8)};
        boards.season.you = boards.season.top[4];
        boards.daily.first_day = boards.daily.last_day = "2026-11-02";
        boards.daily.players = 6;
        boards.daily.top = {entry(1, "Kenji", "", 4, 0, 210, 1), entry(2, "Anosch", "Anosch", 3, 1, 160, 4, true),
                            entry(3, "Lou", "", 2, 2, 95, 2)};
        boards.daily.you = boards.daily.top[1];
        ranked_.boards = boards;
    }
    // The Settings tab: a 16:9 window with the wider view set up, its 3D
    // resolutions; "preview-settings-sound" opens Sound.
    settings_.pictures = {"Keep its 16:9 shape", "Stretch to fill", "Wider view in matches"};
    settings_.resolutions = {"Wii (640 × 448)", "2× (1280 × 896)", "3× (1920 × 1344)", "Window (1920 × 1080)"};
    settings_.resolution = 3;
    settings_.sound_volumes[0] = 70;
    if (const char* tab = std::getenv("RECHARGED_CAPTURE_TAB"))
        for (const auto& entry : state_.tabs)
            if (Slug(entry.label) == Slug(tab)) first = overlay::OverlayTab(entry.id);
    state_.Open(first);
    if (mode_ == "preview-settings-sound") state_.settings_state.section = 1;
    // A capture shows the pictures: they are asked for and loaded before it.
    if (pictures_ && capture_)
    {
        for (const ProfilePlayer* player : {&daisy, &luigi, &rosalina, &bowser, &shy, &koopa})
            pictures_->Get(player->avatar_url);
        pictures_->Settle(std::chrono::seconds(5));
    }
    if (mode_ == "preview-player" || mode_ == "preview-player-mii" || mode_ == "preview-rules-player")
    {
        const bool mii = mode_ == "preview-player-mii";
        state_.online.Push({int(overlay::OnlinePanelScreen::Player), mii ? "m1000000004" : "p900010",
                            mii ? "Tommy" : "Mario"},
                           0);
    }
    // The search's setup ("search", as the panel keys it): Palace tilt, the
    // preset chosen last; "preview-search-running" with the search running.
    if (mode_ == "preview-search-setup" || mode_ == "preview-search-running")
    {
        auto& setup = state_.online_state;
        setup.setup_for = "search";
        setup.preset = 0;
        const auto presets = overlay::MakeOnlinePanelModel(lists_, match_, context_).presets;
        for (std::size_t n = 0; n < presets.size(); ++n)
            if (presets[n].name == "Palace tilt") setup.preset = int(n);
        setup.rules = presets.empty() ? platform::MatchRules{} : presets[std::size_t(setup.preset)].rules;
        state_.online.Push({int(overlay::OnlinePanelScreen::Setup), "search", "Find an opponent"}, 0);
    }
    // A challenge to Mario being set up: Competitive, with Field Tilt.
    if (mode_ == "preview-setup")
    {
        auto& setup = state_.online_state;
        setup.setup_for = "p900010";
        setup.preset = 0;
        setup.rules = context_.rule_sets[0].rules;
        setup.rules.stadium_cheat = "field_tilt";
        state_.online.Push({int(overlay::OnlinePanelScreen::Setup), "p900010", "Challenge Mario"}, 0);
    }
}

ImTextureID OnlinePanelPreview::Avatar(const std::string& key)
{
    const auto found = avatars_.find(key);
    if (found != avatars_.end()) return found->second;
    return pictures_ ? pictures_->Get(key) : ImTextureID(0);
}

// The art as the game's picture under the panel: covering the window,
// blurred (the average of copies around a circle) and darkened.
void OnlinePanelPreview::DrawBackdrop(ImVec2 size, ImTextureID back, ImTextureID front, ImVec2 art_size) const
{
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({0, 0}, size, Col(color::window));
    if (!back || art_size.x <= 0 || art_size.y <= 0) return;
    const float scale = std::max(size.x / art_size.x, size.y / art_size.y) * 1.06f;
    const ImVec2 art{art_size.x * scale, art_size.y * scale};
    const ImVec2 origin{(size.x - art.x) * 0.5f, (size.y - art.y) * 0.45f};
    int taps = 0;
    for (const float radius : {0.0f, 6.0f, 12.0f, 18.0f})
    {
        const int around = radius == 0.0f ? 1 : 8;
        for (int i = 0; i < around; ++i, ++taps)
        {
            const float angle = 6.2831853f * (float(i) + 0.5f * float(taps % 2)) / float(around);
            const ImVec2 at{origin.x + std::cos(angle) * Dp(radius), origin.y + std::sin(angle) * Dp(radius)};
            const ImU32 tint = Col(ImVec4{1, 1, 1, 1}, 1.0f / float(taps + 1));
            // In tiles: SDL's software renderer draws large textured
            // triangles badly (the launcher's header avoids them too).
            constexpr int columns = 8, rows = 6;
            for (int row = 0; row < rows; ++row)
                for (int column = 0; column < columns; ++column)
                {
                    const ImVec2 uv0{float(column) / columns, float(row) / rows};
                    const ImVec2 uv1{float(column + 1) / columns, float(row + 1) / rows};
                    const ImVec2 min{at.x + art.x * uv0.x, at.y + art.y * uv0.y};
                    const ImVec2 max{at.x + art.x * uv1.x, at.y + art.y * uv1.y};
                    if (max.x < 0 || max.y < 0 || min.x > size.x || min.y > size.y) continue;
                    draw->AddImage(back, min, max, uv0, uv1, tint);
                    if (front) draw->AddImage(front, min, max, uv0, uv1, tint);
                }
        }
    }
    // Darker below, as the game's picture dims under a menu.
    draw->AddRectFilledMultiColor({0, 0}, size, Col(color::window, 0.35f), Col(color::window, 0.35f),
                                  Col(color::window, 0.65f), Col(color::window, 0.65f));
}

void OnlinePanelPreview::Draw(ImVec2 size, ImTextureID art_back, ImTextureID art_front, ImVec2 art_size)
{
    DrawBackdrop(size, art_back, art_front, art_size);
    if (mode_.rfind("preview-prompt", 0) == 0)
    {
        DrawPrompt();
        return;
    }
    // F1 opens Online and F2 Game options, as in the game; the open tab's key closes it.
    for (const auto& [key, tab] : {std::pair{ImGuiKey_F1, overlay::OverlayTab::Online},
                                   std::pair{ImGuiKey_F2, overlay::OverlayTab::GameOptions}})
    {
        if (!ImGui::IsKeyPressed(key, false)) continue;
        if (open_ && state_.Tab() == tab) open_ = false;
        else if (open_) state_.Switch(tab);
        else
        {
            open_ = true;
            appear_ = 0.0f;
            state_.Open(tab);
        }
    }
    if (!open_)
    {
        const char* hint = "F1: Online    F2: Game options";
        const ImVec2 text = TextSize(CurrentFonts().heading, hint);
        DrawLabel(ImGui::GetWindowDrawList(), CurrentFonts().heading,
                  {(size.x - text.x) * 0.5f, (size.y - text.y) * 0.5f}, Col(color::text), hint);
        return;
    }
    appear_ = capture_ ? 1.0f : std::min(1.0f, appear_ + ImGui::GetIO().DeltaTime / 0.22f);
    overlay::GameOverlayModel model;
    model.online = overlay::MakeOnlinePanelModel(lists_, match_, context_);
    model.options = options_;
    model.settings = settings_;
    model.ranked = ranked_;
    model.more_tabs = more_tabs_;
    overlay::GameOverlayStyle style;
    style.unit = UiScale();
    style.fonts = CurrentFonts();
    style.appear = appear_;
    style.image = [this](const std::string& key) { return Avatar(key); };
    // A game window's size: the overlay's scale for it (OverlayUnit), the
    // launcher's fonts scaled to it.
    ImGuiIO& io = ImGui::GetIO();
    const float font_scale = io.FontGlobalScale;
    if (game_height_ > 0.0f)
    {
        style.unit = overlay::OverlayUnit(game_width_, game_height_);
        io.FontGlobalScale = font_scale * style.unit / UiScale();
    }
    overlay::GameOverlayActions actions;
    overlay::DrawGameOverlay(model, state_, actions, style);
    io.FontGlobalScale = font_scale;
    if (actions.Any()) Apply(actions);
}

// A prompt as the game shows it after a set or for a challenge; its answers
// go to the console, and it shows again.
void OnlinePanelPreview::DrawPrompt()
{
    using Kind = overlay::OverlayPrompt::Kind;
    overlay::OverlayPrompt prompt;
    const auto person = [&](const char* name, std::int64_t id, const char* image) {
        ProfilePlayer player = Online(Player(id, name, name), "game");
        player.avatar_url = image;
        if (pictures_ && player.avatar_url.empty()) player.avatar_url = DefaultAvatar(id);
        return player;
    };
    const auto row = [](const ProfilePlayer& player) {
        overlay::OnlinePanelRow row;
        row.name = player.name;
        row.image = player.avatar_url;
        row.initials = overlay::Initials(player.name);
        row.tint_seed = player.id;
        row.presence = overlay::Presence::Free;
        row.status = "In the game";
        row.tone = overlay::Tone::Good;
        return row;
    };
    const std::string rules =
        "Competitive: 3 minutes, best of 3, random stadium, Fast terrain, No Mega Strikes";
    if (mode_ == "preview-prompt-lost")
    {
        prompt.kind = Kind::SetOver;
        prompt.kicker = "SET OVER";
        prompt.title = "You lost the set";
        prompt.opponent = row(person("Mario", 900010, "preview:mario"));
        prompt.has_score = true;
        prompt.won = 1;
        prompt.lost = 2;
        prompt.games = 3;
        prompt.detail = rules;
        prompt.primary = "Rematch";
        prompt.secondary = "Not now";
        prompt.note = "A rematch asks Mario to play the same set again.";
    }
    else if (mode_ == "preview-prompt-stopped")
    {
        prompt.kind = Kind::SetStopped;
        prompt.kicker = "SET STOPPED";
        prompt.title = "The set stopped";
        prompt.opponent = row(person("Daisy", 900001, ""));
        prompt.opponent.status = "Left the game";
        prompt.opponent.tone = overlay::Tone::Busy;
        prompt.opponent.presence = overlay::Presence::Busy;
        prompt.has_score = true;
        prompt.won = 1;
        prompt.lost = 1;
        prompt.games = 3;
        prompt.detail = "The connection to Daisy was lost in game 3. Nothing was counted.";
        prompt.primary = "Play it again";
        prompt.secondary = "Not now";
    }
    else if (mode_ == "preview-prompt-challenge")
    {
        prompt.kind = Kind::Challenge;
        prompt.kicker = "CHALLENGE";
        prompt.title = "Peach challenges you";
        prompt.opponent = row(person("Peach", 900002, "preview:peach"));
        prompt.opponent.status = "In the launcher";
        prompt.detail = rules;
        prompt.primary = "Accept";
        prompt.secondary = "Decline";
        prompt.note = "Accept, and your game goes straight to the set.";
    }
    else if (mode_ == "preview-prompt-rematch")
    {
        prompt.kind = Kind::Rematch;
        prompt.kicker = "REMATCH";
        prompt.title = "Koopa wants a rematch";
        prompt.opponent = row(person("Koopa", 900020, ""));
        prompt.has_score = true;
        prompt.won = 2;
        prompt.lost = 1;
        prompt.games = 3;
        prompt.detail = "The same set again: " + rules + ".";
        prompt.primary = "Accept";
        prompt.secondary = "Decline";
    }
    else
    {
        prompt.kind = Kind::SetOver;
        prompt.kicker = "SET OVER";
        prompt.title = "You won the set";
        prompt.opponent = row(person("Koopa", 900020, ""));
        prompt.has_score = true;
        prompt.won = 2;
        prompt.lost = 1;
        prompt.games = 3;
        prompt.detail = rules;
        prompt.primary = "Rematch";
        prompt.secondary = "Not now";
        prompt.note = "A rematch asks Koopa to play the same set again.";
    }
    prompt.image = [this](const std::string& key) { return Avatar(key); };
    overlay::OverlayStyle style;
    style.unit = UiScale();
    style.fonts = CurrentFonts();
    // A game window's size: its scale, as for the overlay.
    ImGuiIO& io = ImGui::GetIO();
    const float font_scale = io.FontGlobalScale;
    if (game_height_ > 0.0f)
    {
        style.unit = overlay::OverlayUnit(game_width_, game_height_);
        io.FontGlobalScale = font_scale * style.unit / UiScale();
    }
    if (capture_ && prompt_state_.shown < 1.0f) prompt_state_.shown = 1.0f; // captures show it in place
    overlay::OverlayPromptActions actions;
    overlay::DrawOverlayPrompt(prompt, prompt_state_, actions, style);
    io.FontGlobalScale = font_scale;
    if (actions.primary || actions.secondary)
    {
        std::cout << "Prompt: " << (actions.primary ? prompt.primary : prompt.secondary) << '\n';
        prompt_state_.Open();
    }
}

// What a press does to the made-up data, as the server and the host would answer it.
void OnlinePanelPreview::Apply(const overlay::GameOverlayActions& all)
{
    // Play RANKED: on the way there; Stop: back.
    if (all.ranked.play) ranked_.going = true, ranked_.can_play = false;
    if (all.ranked.stop) ranked_.going = false, ranked_.can_play = true;
    const overlay::OnlinePanelActions& actions = all.online;
    const auto find = [this](std::int64_t id) -> const ProfilePlayer* {
        for (const auto* list : {&lists_.friends, &lists_.available, &context_.discord_friends})
            for (const auto& player : *list)
                if (player.id == id) return &player;
        return nullptr;
    };
    const auto befriend = [this](std::int64_t id, bool on) {
        for (auto* list : {&lists_.friends, &lists_.available, &context_.discord_friends, &lists_.recent})
            for (auto& player : *list)
                if (player.id == id) player.is_friend = on;
    };
    if (all.close)
    {
        std::cout << "Overlay: close\n";
        open_ = false;
    }
    if (all.set_setting)
    {
        // The preview's settings follow the change.
        const auto& change = *all.set_setting;
        using overlay::GameSetting;
        switch (change.setting)
        {
        case GameSetting::Fullscreen: settings_.fullscreen = change.value; break;
        case GameSetting::Picture: settings_.picture = change.value; break;
        case GameSetting::Resolution: settings_.resolution = change.value; break;
        case GameSetting::PerformanceNumbers: settings_.performance_numbers = change.value; break;
        case GameSetting::MasterVolume: settings_.master_volume = change.value; break;
        case GameSetting::Mute: settings_.mute = change.value; break;
        case GameSetting::SoundVolume: settings_.sound_volumes[std::size_t(change.group)] = change.value; break;
        case GameSetting::ChallengeSound: settings_.challenge_sound = change.value; break;
        case GameSetting::ReopenLauncher: settings_.reopen_launcher = change.value; break;
        }
        std::cout << "Overlay: setting " << int(change.setting) << " = " << change.value << '\n';
    }
    if (actions.search)
    {
        std::cout << "Overlay: search by " << actions.search->name << '\n';
        platform::ProfileSearch search;
        search.rule_set = actions.search->name;
        search.rules = actions.search->rules;
        context_.search = search;
    }
    if (actions.stop_search)
    {
        std::cout << "Overlay: stop the search\n";
        context_.search.reset();
    }
    if (all.set_option)
    {
        const auto& info = kGameTweaks[GameTweakIndex(all.set_option->tweak)];
        std::cout << "Overlay: " << info.name << (all.set_option->on ? " on" : " off") << '\n';
        options_.tweaks[GameTweakIndex(all.set_option->tweak)] = all.set_option->on;
    }
    if (actions.challenge)
    {
        const auto& challenge = *actions.challenge;
        std::cout << "Overlay: challenge player " << challenge.player << " by " << challenge.name
                  << (challenge.rule_set ? " (the server's)" : "") << ": " << platform::DescribeRules(challenge.rules)
                  << '\n';
        if (const auto* player = find(challenge.player)) match_ = Set(*player, "challenged", "challenger", "invited");
        context_.score.reset();
    }
    if (actions.add_friend)
    {
        std::cout << "Overlay: add friend " << *actions.add_friend << '\n';
        if (const auto* player = find(*actions.add_friend))
        {
            ProfilePlayer added = *player;
            added.is_friend = true;
            lists_.friends.push_back(added);
        }
        befriend(*actions.add_friend, true);
    }
    if (actions.remove_friend)
    {
        std::cout << "Overlay: remove friend " << *actions.remove_friend << '\n';
        const std::int64_t id = *actions.remove_friend;
        lists_.friends.erase(std::remove_if(lists_.friends.begin(), lists_.friends.end(),
                                            [id](const ProfilePlayer& player) { return player.id == id; }),
                             lists_.friends.end());
        befriend(id, false);
    }
    const auto mii = [this](std::int32_t id, bool on) {
        for (auto* list : {&lists_.game_online, &lists_.mii_friends})
            for (auto& profile : *list)
                if (profile.profile_id == id) profile.is_friend = on;
        if (on)
        {
            for (const auto& profile : lists_.game_online)
                if (profile.profile_id == id) lists_.mii_friends.push_back(profile);
        }
        else
        {
            const auto gone = [id](const MiiProfile& profile) { return profile.profile_id == id; };
            lists_.mii_friends.erase(std::remove_if(lists_.mii_friends.begin(), lists_.mii_friends.end(), gone),
                                     lists_.mii_friends.end());
        }
    };
    if (actions.add_mii_friend)
    {
        std::cout << "Overlay: add Mii friend " << *actions.add_mii_friend << '\n';
        mii(*actions.add_mii_friend, true);
    }
    if (actions.remove_mii_friend)
    {
        std::cout << "Overlay: remove Mii friend " << *actions.remove_mii_friend << '\n';
        mii(*actions.remove_mii_friend, false);
    }
    if (actions.set_challenge_rules)
    {
        std::cout << "Overlay: challenges by " << *actions.set_challenge_rules << '\n';
        context_.chosen_rules = *actions.set_challenge_rules;
    }
    if (actions.set_available)
    {
        std::cout << "Overlay: available for random games " << (*actions.set_available ? "on" : "off") << '\n';
        context_.me.available = *actions.set_available;
    }
    if (match_)
    {
        if (actions.accept)
        {
            std::cout << "Overlay: accept set " << match_->id << '\n';
            match_->state = "accepted";
            match_->stage = "open";
            match_->you_ready = true;
        }
        if (actions.ready)
        {
            std::cout << "Overlay: ready for set " << match_->id << '\n';
            match_->you_ready = true;
        }
        if (actions.rematch)
        {
            std::cout << "Overlay: rematch of set " << match_->id << '\n';
            match_ = Set(match_->opponent, "challenged", "challenger", "invited");
        }
        if (actions.decline || actions.cancel)
        {
            std::cout << "Overlay: " << (actions.decline ? "decline" : "cancel") << " set " << match_->id << '\n';
            match_.reset();
        }
    }
}
} // namespace mscharged::launcher
