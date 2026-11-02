#include "overlay/ranked_tab.h"

#include "launcher/ui_kit.h"

#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdlib>

namespace mscharged::overlay
{
namespace
{
namespace color = launcher::color;
using launcher::Col;
using launcher::Dp;
using launcher::DrawIcon;
using launcher::DrawLabel;
using launcher::FontSize;
using launcher::Icon;
using launcher::Mix;
using launcher::TextSize;
using platform::LeaderboardBoard;
using platform::LeaderboardEntry;

// Text wrapped to `width`, and its height.
float Wrapped(ImDrawList* draw, ImFont* font, ImVec2 at, const ImVec4& tint, const char* text, float width)
{
    draw->AddText(font, FontSize(font), at, Col(tint), text, nullptr, width);
    return font->CalcTextSizeA(FontSize(font), FLT_MAX, width, text).y;
}

// "2026-11-02" as "Nov 2".
std::string Day(const std::string& date)
{
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (date.size() != 10) return date;
    const int month = std::atoi(date.substr(5, 2).c_str()), day = std::atoi(date.substr(8, 2).c_str());
    return month >= 1 && month <= 12 ? std::string(kMonths[month - 1]) + " " + std::to_string(day) : date;
}

// The first three in gold, silver and bronze.
ImVec4 RankTint(int rank)
{
    static const ImVec4 kMedals[] = {{1.0f, 0.80f, 0.30f, 1}, {0.80f, 0.84f, 0.90f, 1}, {0.86f, 0.58f, 0.36f, 1}};
    return rank >= 1 && rank <= 3 ? kMedals[rank - 1] : color::muted;
}

bool Pressed(ImGuiKey key, ImGuiKey pad, ImGuiKey stick)
{
    return ImGui::IsKeyPressed(key) || ImGui::IsKeyPressed(pad) || ImGui::IsKeyPressed(stick);
}

// Play RANKED: what it is, its button, and what happens.
void DrawPlay(const RankedModel& model, RankedState& state, RankedActions& actions, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = Dp(20), inner = width - pad * 2;
    const char* about = "Play anybody online, as from the game's online menu. Your wins and losses go on the "
                        "leaderboards.";
    const std::string status = !model.blocked.empty() ? model.blocked
        : model.waiting  ? "The game goes to RANKED from the main menu or the online menu: go back there."
        : model.going    ? "The game is on its way to RANKED."
        : model.own_mii  ? "With your own Mii, for one player."
                         : "You choose your Mii in the game, for one player.";
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float about_h = fonts.body->CalcTextSizeA(FontSize(fonts.body), FLT_MAX, inner, about).y;
    const float status_h = fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, inner, status.c_str()).y;
    const float badge = Dp(44), button = Dp(52);
    const float height = pad + badge + Dp(14) + FontSize(fonts.heading) + Dp(6) + about_h + Dp(18) + button + Dp(10)
        + status_h + pad;
    const ImVec2 end{at.x + width, at.y + height};
    draw->AddRectFilled(at, end, Col(color::field, 0.85f), Dp(16));
    draw->AddRectFilledMultiColor({at.x + Dp(1), at.y + Dp(1)}, {end.x - Dp(1), at.y + Dp(110)},
                                  Col(color::accent, 0.10f), Col(color::accent, 0.10f), Col(color::accent, 0.0f),
                                  Col(color::accent, 0.0f));
    draw->AddRect(at, end, Col(color::border, 0.8f), Dp(16));
    float y = at.y + pad;
    const ImVec2 icon{at.x + pad + badge * 0.5f, y + badge * 0.5f};
    draw->AddCircleFilled(icon, badge * 0.5f, Col(color::accent, 0.16f));
    DrawIcon(draw, Icon::Podium, icon, Dp(22), Col(color::accent));
    y += badge + Dp(14);
    DrawLabel(draw, fonts.heading, {at.x + pad, y}, Col(color::text), "RANKED");
    y += FontSize(fonts.heading) + Dp(6);
    y += Wrapped(draw, fonts.body, {at.x + pad, y}, color::muted, about, inner) + Dp(18);
    ImGui::SetCursorScreenPos({at.x + pad, y});
    if (model.going)
    {
        if (OverlayButton("ranked_stop", "Stop", Icon::Close, {inner, button}, OverlayLook::Secondary, true, true))
            actions.stop = true;
    }
    else if (OverlayButton("ranked_play", "Play RANKED", Icon::Play, {inner, button}, OverlayLook::Primary,
                           model.can_play, true))
        actions.play = true;
    state.play_id = ImGui::GetItemID();
    state.play_window = ImGui::GetCurrentWindow();
    if (OverlayNavFocused() && model.boards && !model.signed_out
        && Pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight))
        state.jump = RankedState::Jump::ToBoards;
    y += button + Dp(10);
    Wrapped(draw, fonts.caption, {at.x + pad, y}, model.blocked.empty() ? color::muted : color::warning,
            status.c_str(), inner);
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({width, height});
}

// The player's own places: the season's and today's.
void DrawMine(const RankedModel& model, float width)
{
    if (model.signed_out || !model.boards) return;
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    DrawLabel(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(color::dim), "MY RANKING");
    ImGui::Dummy({width, FontSize(fonts.caption) + Dp(8)});
    const float gap = Dp(10), tile = (width - gap) * 0.5f, pad = Dp(14);
    const float height = pad + FontSize(fonts.caption) + Dp(6) + FontSize(fonts.heading) + Dp(4)
        + FontSize(fonts.caption) + pad;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const std::pair<const char*, const std::optional<LeaderboardEntry>*> places[] = {
        {"THIS SEASON", &model.boards->season.you}, {"TODAY", &model.boards->daily.you}};
    for (int n = 0; n < 2; ++n)
    {
        const ImVec2 min{at.x + float(n) * (tile + gap), at.y};
        const ImVec2 max{min.x + tile, min.y + height};
        draw->AddRectFilled(min, max, Col(color::field, 0.7f), Dp(14));
        draw->AddRect(min, max, Col(color::border, 0.8f), Dp(14));
        float y = min.y + pad;
        DrawLabel(draw, fonts.caption, {min.x + pad, y}, Col(color::dim), places[n].first);
        y += FontSize(fonts.caption) + Dp(6);
        const auto& you = *places[n].second;
        if (!you)
        {
            DrawLabel(draw, fonts.body, {min.x + pad, y + (FontSize(fonts.heading) - FontSize(fonts.body)) * 0.5f},
                      Col(color::muted), "Not ranked yet");
            continue;
        }
        const std::string rank = "#" + std::to_string(you->rank);
        DrawLabel(draw, fonts.heading, {min.x + pad, y}, Col(RankTint(you->rank)), rank.c_str());
        const std::string points = std::to_string(you->points) + " pts";
        DrawLabel(draw, fonts.label,
                  {max.x - pad - TextSize(fonts.label, points.c_str()).x,
                   y + (FontSize(fonts.heading) - FontSize(fonts.label)) * 0.5f},
                  Col(color::text), points.c_str());
        y += FontSize(fonts.heading) + Dp(4);
        const std::string record = (you->wins ? std::to_string(*you->wins) : std::string("-")) + " won  ·  "
            + (you->losses ? std::to_string(*you->losses) : std::string("-")) + " lost";
        DrawLabel(draw, fonts.caption, {min.x + pad, y}, Col(color::muted), record.c_str());
    }
    ImGui::Dummy({width, height});
}

// Season leaders | Daily leaders: the board shown, as one control. A click
// picks one; with the cursor on it left and right switch (left from the
// season's back to Play RANKED), A the other.
void DrawBoardChoice(RankedState& state, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const char* kNames[] = {"Season leaders", "Daily leaders"};
    const float height = Dp(36), gap = Dp(6);
    const float each = std::min((width - gap) * 0.5f, Dp(170));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 end{at.x + each * 2 + gap, at.y + height};
    if (state.jump == RankedState::Jump::ToBoards)
    {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
        state.jump = RankedState::Jump::None;
    }
    const bool pressed = ImGui::InvisibleButton("##boards", {end.x - at.x, height}, ImGuiButtonFlags_EnableNav);
    const ImGuiID item = ImGui::GetItemID();
    const bool focused = OverlayNavFocused();
    OverlayFocusable();
    const float mouse = ImGui::GetIO().MousePos.x;
    const int hovered = ImGui::IsItemHovered() ? (mouse < at.x + each + gap * 0.5f ? 0 : 1) : -1;
    if (pressed) state.board = hovered >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) ? hovered : 1 - state.board;
    if (focused)
    {
        for (const ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft,
                                   ImGuiKey_GamepadDpadRight})
            ImGui::SetKeyOwner(key, item);
        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft);
        const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight);
        if (left && state.board == 0 && state.play_id && state.play_window)
        {
            ImGui::SetFocusID(state.play_id, state.play_window);
            ImGui::SetNavCursorVisible(true);
        }
        else if (left) state.board = 0;
        else if (right) state.board = 1;
    }
    for (int n = 0; n < 2; ++n)
    {
        const ImVec2 min{at.x + float(n) * (each + gap), at.y};
        const ImVec2 max{min.x + each, min.y + height};
        const bool chosen = state.board == n;
        ImVec4 fill = chosen ? Mix(color::field, color::accent_dark, 0.6f) : color::field;
        fill.w = chosen ? 0.95f : hovered == n ? 0.7f : 0.45f;
        draw->AddRectFilled(min, max, Col(fill), height * 0.5f);
        if (chosen) draw->AddRect(min, max, Col(color::accent, 0.55f), height * 0.5f, 0, Dp(1));
        const ImVec2 size = TextSize(fonts.label, kNames[n]);
        DrawLabel(draw, fonts.label, {min.x + (each - size.x) * 0.5f, min.y + (height - size.y) * 0.5f},
                  Col(chosen ? color::text : color::muted), kNames[n]);
    }
    if (focused)
    {
        const ImVec2 min{at.x + float(state.board) * (each + gap), at.y};
        OverlayFocusRing(draw, min, {min.x + each, min.y + height}, height * 0.5f);
    }
    ImGui::SetCursorScreenPos({at.x, end.y});
    ImGui::Dummy({width, 0.0f});
}

// A row's name: the Mii, its player when another name, its region; clipped
// before `right`.
void DrawName(ImDrawList* draw, const LeaderboardEntry& entry, ImVec2 at, float right, bool centred)
{
    const auto& fonts = launcher::CurrentFonts();
    const std::string mii = entry.mii.empty() ? std::string("?") : entry.mii;
    const char* region = entry.region == 1 ? "JP" : entry.region == 2 ? "US" : entry.region == 4 ? "EU"
        : entry.region == 8                ? "KR"
                                           : nullptr;
    const float mii_w = TextSize(fonts.label, mii.c_str()).x;
    const float region_w = region ? TextSize(fonts.caption, region).x + Dp(12) + Dp(8) : 0.0f;
    float x = centred ? std::max(at.x, (at.x + right - mii_w - region_w) * 0.5f) : at.x;
    draw->PushClipRect({at.x, at.y - Dp(4)}, {right, at.y + Dp(40)}, true);
    DrawLabel(draw, fonts.label, {x, at.y}, Col(entry.you ? color::accent : color::text), mii.c_str());
    x += mii_w + Dp(8);
    if (region)
    {
        const ImVec2 tag = TextSize(fonts.caption, region);
        const float top = at.y + (FontSize(fonts.label) - Dp(16)) * 0.5f;
        draw->AddRectFilled({x, top}, {x + tag.x + Dp(12), top + Dp(16)}, Col(color::border, 0.8f), Dp(5));
        DrawLabel(draw, fonts.caption, {x + Dp(6), top + (Dp(16) - tag.y) * 0.5f}, Col(color::muted), region);
    }
    draw->PopClipRect();
}

// "17-10", and how many of its games it won as a bar under it.
void DrawRecord(ImDrawList* draw, const LeaderboardEntry& entry, ImVec2 center, float bar)
{
    const auto& fonts = launcher::CurrentFonts();
    const std::string record = (entry.wins ? std::to_string(*entry.wins) : std::string("-")) + "\xE2\x80\x93"
        + (entry.losses ? std::to_string(*entry.losses) : std::string("-"));
    const ImVec2 size = TextSize(fonts.body, record.c_str());
    DrawLabel(draw, fonts.body, {center.x - size.x * 0.5f, center.y - size.y}, Col(color::text), record.c_str());
    const int played = entry.wins.value_or(0) + entry.losses.value_or(0);
    const ImVec2 min{center.x - bar * 0.5f, center.y + Dp(4)};
    draw->AddRectFilled(min, {min.x + bar, min.y + Dp(3)}, Col(color::danger, 0.35f), Dp(2));
    if (played > 0)
        draw->AddRectFilled(min, {min.x + bar * float(entry.wins.value_or(0)) / float(played), min.y + Dp(3)},
                            Col(color::accent, 0.85f), Dp(2));
}

// The first three on a podium: second, first, third, the first highest, each
// in its medal's colour.
void DrawPodium(const LeaderboardBoard& board, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const int count = int(std::min<std::size_t>(board.top.size(), 3));
    const float gap = Dp(10), each = (width - gap * 2) / 3, top = Dp(176), step = Dp(10);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float floor = at.y + top;
    // Places from the left: second, first, third.
    constexpr int kOrder[3] = {1, 0, 2};
    for (int slot = 0; slot < 3; ++slot)
    {
        const int place = kOrder[slot];
        if (place >= count) continue;
        const LeaderboardEntry& entry = board.top[std::size_t(place)];
        const ImVec4 medal = RankTint(entry.rank);
        const float height = top - (place == 0 ? 0.0f : step) - (place == 2 ? step : 0.0f);
        const ImVec2 min{at.x + float(slot) * (each + gap), floor - height};
        const ImVec2 max{min.x + each, floor};
        // The first glows.
        if (place == 0)
            for (int ring = 3; ring >= 1; --ring)
                draw->AddRect({min.x - float(ring) * Dp(2), min.y - float(ring) * Dp(2)},
                              {max.x + float(ring) * Dp(2), max.y + float(ring) * Dp(2)},
                              Col(medal, 0.06f * float(4 - ring)), Dp(14) + float(ring) * Dp(2), 0, Dp(2));
        launcher::FillGradient(draw, min, max, Col(Mix(color::field, medal, place == 0 ? 0.30f : 0.20f)),
                               Col(color::field), Dp(14), false);
        draw->AddRect(min, max, Col(entry.you ? color::accent : medal, entry.you ? 0.9f : 0.55f), Dp(14), 0,
                      Dp(entry.you ? 2.0f : 1.0f));
        // The medal with its place.
        const float radius = place == 0 ? Dp(17) : Dp(14);
        const ImVec2 center{(min.x + max.x) * 0.5f, min.y + Dp(14) + radius};
        draw->AddCircleFilled(center, radius + Dp(3), Col(medal, 0.20f));
        draw->AddCircleFilled(center, radius, Col(medal));
        draw->AddCircle(center, radius - Dp(2.5f), Col(ImVec4{1, 1, 1, 0.45f}), 0, Dp(1));
        const std::string rank = std::to_string(entry.rank);
        const ImVec2 rank_size = TextSize(fonts.heading, rank.c_str());
        DrawLabel(draw, fonts.heading, {center.x - rank_size.x * 0.5f, center.y - rank_size.y * 0.5f},
                  Col(ImVec4{0.10f, 0.08f, 0.04f, 1}), rank.c_str());
        float y = center.y + radius + Dp(10);
        DrawName(draw, entry, {min.x + Dp(10), y}, max.x - Dp(10), true);
        y += FontSize(fonts.label) + Dp(2);
        if (!entry.player.empty() && entry.player != entry.mii)
        {
            const ImVec2 size = TextSize(fonts.caption, entry.player.c_str());
            draw->PushClipRect({min.x + Dp(8), y}, {max.x - Dp(8), y + FontSize(fonts.caption) + Dp(2)}, true);
            DrawLabel(draw, fonts.caption, {std::max(min.x + Dp(8), (min.x + max.x - size.x) * 0.5f), y},
                      Col(color::dim), entry.player.c_str());
            draw->PopClipRect();
        }
        // The points, large, and the record at the foot.
        const std::string points = std::to_string(entry.points);
        const ImVec2 points_size = TextSize(fonts.title, points.c_str());
        const float points_y = max.y - Dp(14) - FontSize(fonts.caption) - Dp(4) - points_size.y;
        const float pts_w = TextSize(fonts.caption, " pts").x;
        const float points_x = (min.x + max.x - points_size.x - pts_w) * 0.5f;
        DrawLabel(draw, fonts.title, {points_x, points_y}, Col(color::text), points.c_str());
        DrawLabel(draw, fonts.caption,
                  {points_x + points_size.x, points_y + points_size.y - FontSize(fonts.caption) - Dp(3)},
                  Col(color::muted), " pts");
        const std::string record = (entry.wins ? std::to_string(*entry.wins) : std::string("-")) + " won  \xC2\xB7  "
            + (entry.losses ? std::to_string(*entry.losses) : std::string("-")) + " lost";
        const ImVec2 record_size = TextSize(fonts.caption, record.c_str());
        DrawLabel(draw, fonts.caption, {(min.x + max.x - record_size.x) * 0.5f, max.y - Dp(12) - record_size.y},
                  Col(color::muted), record.c_str());
    }
    ImGui::Dummy({width, top});
}

// One row of the table: the rank's badge, the name, the record, the points
// with a bar against the leader's.
void DrawRow(const LeaderboardEntry& entry, int leader, float width, bool pinned)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(44), middle = at.y + height * 0.5f;
    const ImVec2 end{at.x + width, at.y + height};
    const bool hovered = ImGui::IsMouseHoveringRect(at, end) && ImGui::IsWindowHovered();
    if (entry.you)
    {
        draw->AddRectFilled(at, end, Col(color::accent, 0.12f), Dp(12));
        draw->AddRect(at, end, Col(color::accent, 0.6f), Dp(12), 0, Dp(1.5f));
    }
    else if (hovered) draw->AddRectFilled(at, end, Col(color::text, 0.05f), Dp(12));
    else if (!pinned) draw->AddRectFilled(at, end, Col(color::field, 0.35f), Dp(12));
    // The rank in a badge.
    const std::string rank = std::to_string(entry.rank);
    const ImVec2 rank_size = TextSize(fonts.label, rank.c_str());
    const float badge_w = std::max(Dp(34), rank_size.x + Dp(14));
    const ImVec2 badge{at.x + Dp(8), middle - Dp(13)};
    draw->AddRectFilled(badge, {badge.x + badge_w, badge.y + Dp(26)}, Col(color::text, 0.07f), Dp(8));
    DrawLabel(draw, fonts.label, {badge.x + (badge_w - rank_size.x) * 0.5f, middle - rank_size.y * 0.5f},
              Col(color::muted), rank.c_str());
    // The name (and its player), then YOU.
    const float points_w = Dp(76), record_w = Dp(70);
    const float name_x = badge.x + badge_w + Dp(12), name_right = at.x + width - points_w - record_w - Dp(12);
    const bool player = !entry.player.empty() && entry.player != entry.mii;
    const float name_y = player ? middle - FontSize(fonts.label) + Dp(1) : middle - FontSize(fonts.label) * 0.5f;
    DrawName(draw, entry, {name_x, name_y}, name_right - (entry.you ? Dp(44) : 0.0f), false);
    if (player)
    {
        draw->PushClipRect({name_x, middle}, {name_right, at.y + height}, true);
        DrawLabel(draw, fonts.caption, {name_x, middle + Dp(2)}, Col(color::dim), entry.player.c_str());
        draw->PopClipRect();
    }
    if (entry.you)
    {
        const ImVec2 tag = TextSize(fonts.caption, "YOU");
        const ImVec2 tag_at{name_right - tag.x - Dp(14), middle - Dp(9)};
        draw->AddRectFilled(tag_at, {tag_at.x + tag.x + Dp(14), tag_at.y + Dp(18)}, Col(color::accent, 0.9f), Dp(9));
        DrawLabel(draw, fonts.caption, {tag_at.x + Dp(7), tag_at.y + (Dp(18) - tag.y) * 0.5f},
                  Col(color::accent_ink), "YOU");
    }
    // The record, and the points against the leader's.
    DrawRecord(draw, entry, {at.x + width - points_w - record_w * 0.5f, middle + Dp(3)}, Dp(44));
    const std::string points = std::to_string(entry.points);
    const ImVec2 size = TextSize(fonts.label, points.c_str());
    const float right = at.x + width - Dp(12);
    DrawLabel(draw, fonts.label, {right - size.x, middle - size.y + Dp(2)}, Col(color::text), points.c_str());
    const float bar = points_w - Dp(20);
    const float share = leader > 0 ? std::clamp(float(entry.points) / float(leader), 0.0f, 1.0f) : 0.0f;
    draw->AddRectFilled({right - bar, middle + Dp(6)}, {right, middle + Dp(9)}, Col(color::text, 0.08f), Dp(2));
    draw->AddRectFilled({right - bar * share, middle + Dp(6)}, {right, middle + Dp(9)}, Col(color::accent, 0.6f),
                        Dp(2));
    ImGui::Dummy({width, height});
}

// One board: its days and players, the podium, the others as a table, and
// the player's own row at the end when it isn't among them.
void DrawBoard(const LeaderboardBoard& board, bool daily, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const std::string when = (daily ? "Today, " + Day(board.first_day) : Day(board.first_day) + " - "
                                                                              + Day(board.last_day))
        + "  \xC2\xB7  " + std::to_string(board.players) + (board.players == 1 ? " player" : " players");
    DrawLabel(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(color::muted), when.c_str());
    ImGui::Dummy({width, FontSize(fonts.caption) + Dp(10)});
    if (board.top.empty())
    {
        DrawLabel(draw, fonts.body, ImGui::GetCursorScreenPos(), Col(color::muted),
                  daily ? "Nobody has played RANKED today yet." : "Nobody has played RANKED this season yet.");
        ImGui::Dummy({width, FontSize(fonts.body)});
        return;
    }
    // The podium needs room for three cards; else everyone in the table.
    const bool podium = width >= Dp(360);
    std::size_t first = 0;
    if (podium)
    {
        ImGui::Dummy({width, Dp(6)});
        DrawPodium(board, width);
        ImGui::Dummy({width, Dp(12)});
        first = std::min<std::size_t>(board.top.size(), 3);
    }
    const int leader = board.top.front().points;
    if (first < board.top.size())
    {
        // The column heads.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const auto head = [&](float x, const char* text, bool right) {
            const float w = launcher::TrackedWidth(fonts.caption, text, Dp(1.2f));
            launcher::DrawTracked(draw, fonts.caption, {right ? x - w : x - w * 0.5f, at.y}, Col(color::dim), text,
                                  Dp(1.2f));
        };
        launcher::DrawTracked(draw, fonts.caption, {at.x + Dp(8), at.y}, Col(color::dim), "RANK", Dp(1.2f));
        launcher::DrawTracked(draw, fonts.caption, {at.x + Dp(62), at.y}, Col(color::dim), "PLAYER", Dp(1.2f));
        head(at.x + width - Dp(76) - Dp(35), "W / L", false);
        head(at.x + width - Dp(12), "POINTS", true);
        ImGui::Dummy({width, FontSize(fonts.caption) + Dp(4)});
    }
    bool shown = false;
    for (std::size_t n = first; n < board.top.size(); ++n)
    {
        DrawRow(board.top[n], leader, width, false);
        shown |= board.top[n].you;
    }
    for (std::size_t n = 0; n < first; ++n) shown |= board.top[n].you;
    if (board.you && !shown)
    {
        // Further down: the player's own row, after a gap.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        for (int dot = 0; dot < 3; ++dot)
            draw->AddCircleFilled({at.x + width * 0.5f + float(dot - 1) * Dp(8), at.y + Dp(8)}, Dp(1.8f),
                                  Col(color::dim));
        ImGui::Dummy({width, Dp(16)});
        DrawRow(*board.you, leader, width, true);
    }
}

// The leaderboards, or why there are none here.
void DrawBoards(const RankedModel& model, RankedState& state, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const char* none = model.signed_out
        ? "The leaderboards show here with a player profile: make one on the launcher's Player profile page."
        : model.missing ? "This server has no leaderboards for Recharged yet."
        : !model.boards ? "Loading the leaderboards..."
                        : nullptr;
    if (none)
    {
        const float h = Wrapped(draw, fonts.body, ImGui::GetCursorScreenPos(), color::muted, none, width);
        ImGui::Dummy({width, h});
        return;
    }
    state.board = std::clamp(state.board, 0, 1);
    DrawBoardChoice(state, width);
    ImGui::Dummy({width, Dp(12)});
    const bool daily = state.board == 1;
    DrawBoard(daily ? model.boards->daily : model.boards->season, daily, width);
}
} // namespace

OverlayScreens::Screen RankedFirstScreen() { return {0, {}, "Ranked"}; }

void DrawRanked(const RankedModel& model, RankedState& state, RankedActions& actions, const OverlayLayout& layout)
{
    const float width = layout.max.x - layout.min.x;
    const bool wide = width >= Dp(700);
    OverlayLayoutIs(wide ? 2 : 1);
    if (wide)
    {
        // Play and the player's places at the left, the leaders at the right.
        const float left = OverlaySplitLeft(width);
        BeginOverlayScroll("##ranked_play", layout.min, {layout.min.x + left, layout.max.y});
        ImGui::Dummy({left, Dp(20)});
        DrawPlay(model, state, actions, left);
        if (!model.signed_out && model.boards)
        {
            ImGui::Dummy({left, Dp(18)});
            DrawMine(model, left);
        }
        ImGui::Dummy({left, Dp(16)});
        EndOverlayScroll();
        OverlaySplitDivider(layout, left);
        const ImVec2 pane{layout.min.x + left + OverlaySplitGap(), layout.min.y};
        BeginOverlayScroll("##ranked_boards", pane, layout.max);
        ImGui::Dummy({layout.max.x - pane.x, Dp(20)});
        DrawBoards(model, state, layout.max.x - pane.x);
        ImGui::Dummy({layout.max.x - pane.x, Dp(16)});
        EndOverlayScroll();
        return;
    }
    BeginOverlayScroll("##ranked_column", layout.min, layout.max);
    ImGui::Dummy({width, Dp(18)});
    DrawPlay(model, state, actions, width);
    if (!model.signed_out && model.boards)
    {
        ImGui::Dummy({width, Dp(16)});
        DrawMine(model, width);
    }
    ImGui::Dummy({width, Dp(18)});
    DrawBoards(model, state, width);
    ImGui::Dummy({width, Dp(16)});
    EndOverlayScroll();
}
} // namespace mscharged::overlay
