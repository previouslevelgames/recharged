#include "launcher/online_panel_preview.h"

#include "launcher/ui_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>

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
} // namespace

OnlinePanelPreview::OnlinePanelPreview(SDL_Renderer* renderer, bool capture) : capture_(capture)
{
    const char* mode = std::getenv("RECHARGED_CAPTURE_DISCORD");
    mode_ = mode && std::string(mode).rfind("preview", 0) == 0 ? mode : "preview";
    const auto now = std::int64_t(std::time(nullptr));
    context_.now = now;
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
        if (SDL_Texture* texture = renderer ? MakeAvatar(renderer, look.top, look.bottom, look.shirt, look.hair) : nullptr)
            avatars_[look.key] = TextureId(texture);

    ProfilePlayer& me = context_.me;
    me = Player(900000, "Anosch", "anosch");
    me.avatar_url = "preview:me";
    me.available = mode_ != "preview-empty";

    auto mario = Online(Player(900010, "Mario", "itsamario"), "launcher");
    mario.avatar_url = "preview:mario";
    mario.is_friend = mario.chat = true;
    mario.unread = 2;
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
    koopa.last_online = now - 26 * 3600;

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
    if (mode_ == "preview")
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
        context_.score = online_panel::OnlinePanelContext::Score{1, 0};
    }
    else if (mode_ == "preview-invite") match_ = Set(peach, "challenged", "challenged", "invited");
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
    else if (mode_ == "preview-offline") state_.offline_open = true;
    else if (mode_ == "preview-match") context_.in_match = true;
}

ImTextureID OnlinePanelPreview::Avatar(const std::string& key) const
{
    const auto found = avatars_.find(key);
    return found == avatars_.end() ? ImTextureID(0) : found->second;
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
    if (!open_)
    {
        const char* hint = "Press F1 to open the online panel";
        const ImVec2 text = TextSize(CurrentFonts().heading, hint);
        DrawLabel(ImGui::GetWindowDrawList(), CurrentFonts().heading,
                  {(size.x - text.x) * 0.5f, (size.y - text.y) * 0.5f}, Col(color::text), hint);
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        {
            open_ = true;
            appear_ = 0.0f;
            state_.focus = true;
        }
        return;
    }
    appear_ = capture_ ? 1.0f : std::min(1.0f, appear_ + ImGui::GetIO().DeltaTime / 0.22f);
    context_.now = std::int64_t(std::time(nullptr));
    const auto model = online_panel::MakeOnlinePanelModel(lists_, match_, context_);
    online_panel::OnlinePanelStyle style;
    style.unit = UiScale();
    style.fonts = CurrentFonts();
    style.appear = appear_;
    style.image = [this](const std::string& key) { return Avatar(key); };
    online_panel::OnlinePanelActions actions;
    online_panel::DrawOnlinePanel(model, state_, actions, style);
    if (actions.Any()) Apply(actions);
}

// What a press does to the made-up players, as the server would answer it.
void OnlinePanelPreview::Apply(const online_panel::OnlinePanelActions& actions)
{
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
    if (actions.close)
    {
        std::cout << "Online panel: close\n";
        open_ = false;
    }
    if (actions.challenge)
    {
        std::cout << "Online panel: challenge player " << *actions.challenge << '\n';
        if (const auto* player = find(*actions.challenge)) match_ = Set(*player, "challenged", "challenger", "invited");
        context_.score.reset();
    }
    if (actions.add_friend)
    {
        std::cout << "Online panel: add friend " << *actions.add_friend << '\n';
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
        std::cout << "Online panel: remove friend " << *actions.remove_friend << '\n';
        const std::int64_t id = *actions.remove_friend;
        lists_.friends.erase(std::remove_if(lists_.friends.begin(), lists_.friends.end(),
                                            [id](const ProfilePlayer& player) { return player.id == id; }),
                             lists_.friends.end());
        befriend(id, false);
    }
    const auto mii = [this](std::int32_t id, bool on) {
        for (auto& profile : lists_.game_online)
            if (profile.profile_id == id)
            {
                profile.is_friend = on;
                if (on) lists_.mii_friends.push_back(profile);
            }
        if (!on)
            lists_.mii_friends.erase(std::remove_if(lists_.mii_friends.begin(), lists_.mii_friends.end(),
                                                    [id](const MiiProfile& profile) { return profile.profile_id == id; }),
                                     lists_.mii_friends.end());
    };
    if (actions.add_mii_friend)
    {
        std::cout << "Online panel: add Mii friend " << *actions.add_mii_friend << '\n';
        mii(*actions.add_mii_friend, true);
    }
    if (actions.remove_mii_friend)
    {
        std::cout << "Online panel: remove Mii friend " << *actions.remove_mii_friend << '\n';
        mii(*actions.remove_mii_friend, false);
    }
    if (actions.set_available)
    {
        std::cout << "Online panel: available for random games " << (*actions.set_available ? "on" : "off") << '\n';
        context_.me.available = *actions.set_available;
    }
    if (match_)
    {
        if (actions.accept)
        {
            std::cout << "Online panel: accept set " << match_->id << '\n';
            match_->state = "accepted";
            match_->stage = "open";
            match_->you_ready = true;
        }
        if (actions.ready)
        {
            std::cout << "Online panel: ready for set " << match_->id << '\n';
            match_->you_ready = true;
        }
        if (actions.rematch)
        {
            std::cout << "Online panel: rematch of set " << match_->id << '\n';
            match_ = Set(match_->opponent, "challenged", "challenger", "invited");
        }
        if (actions.decline || actions.cancel)
        {
            std::cout << "Online panel: " << (actions.decline ? "decline" : "cancel") << " set " << match_->id << '\n';
            match_.reset();
        }
    }
}
} // namespace mscharged::launcher
