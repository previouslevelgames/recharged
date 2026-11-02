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

// One board: its days and players, the column heads, the leaders.
void DrawBoard(const LeaderboardBoard& board, bool daily, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const std::string when = (daily ? "Today, " + Day(board.first_day) : Day(board.first_day) + " - "
                                                                              + Day(board.last_day))
        + "  ·  " + std::to_string(board.players) + (board.players == 1 ? " player" : " players");
    DrawLabel(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(color::muted), when.c_str());
    ImGui::Dummy({width, FontSize(fonts.caption) + Dp(12)});
    if (board.top.empty())
    {
        DrawLabel(draw, fonts.body, ImGui::GetCursorScreenPos(), Col(color::muted),
                  daily ? "Nobody has played RANKED today yet." : "Nobody has played RANKED this season yet.");
        ImGui::Dummy({width, FontSize(fonts.body)});
        return;
    }
    // The columns: rank, the Mii (and its player), won, lost, points.
    const float rank_width = Dp(48), number = Dp(46), points = Dp(64);
    const auto columns = [&](float left) {
        const float points_x = left + width - points;
        return std::array<float, 4>{left + rank_width, points_x - 2 * number, points_x - number, points_x};
    };
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const auto x = columns(at.x);
        const auto head = [&](float left, float right, const char* text) {
            const float w = TextSize(fonts.caption, text).x;
            DrawLabel(draw, fonts.caption, {right > 0 ? right - w : left, at.y}, Col(color::dim), text);
        };
        head(at.x + Dp(8), 0, "RANK");
        head(x[0], 0, "PLAYER");
        head(0, x[2] - Dp(10), "W");
        head(0, x[3] - Dp(10), "L");
        head(0, at.x + width - Dp(8), "PTS");
        ImGui::Dummy({width, FontSize(fonts.caption) + Dp(6)});
    }
    for (const LeaderboardEntry& entry : board.top)
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float height = Dp(38), middle = at.y + height * 0.5f;
        const auto x = columns(at.x);
        if (entry.you) draw->AddRectFilled(at, {at.x + width, at.y + height}, Col(color::accent, 0.12f), Dp(10));
        const std::string rank = "#" + std::to_string(entry.rank);
        DrawLabel(draw, fonts.label, {at.x + Dp(8), middle - FontSize(fonts.label) * 0.5f}, Col(RankTint(entry.rank)),
                  rank.c_str());
        const std::string mii = entry.mii.empty() ? std::string("?") : entry.mii;
        draw->PushClipRect({x[0], at.y}, {x[1] - Dp(8), at.y + height}, true);
        DrawLabel(draw, fonts.label, {x[0], middle - FontSize(fonts.label) * 0.5f},
                  Col(entry.you ? color::accent : color::text), mii.c_str());
        float after = x[0] + TextSize(fonts.label, mii.c_str()).x + Dp(8);
        if (!entry.player.empty() && entry.player != entry.mii)
        {
            DrawLabel(draw, fonts.caption, {after, middle - FontSize(fonts.caption) * 0.5f}, Col(color::dim),
                      entry.player.c_str());
            after += TextSize(fonts.caption, entry.player.c_str()).x + Dp(8);
        }
        const char* region = entry.region == 1 ? "JP" : entry.region == 2 ? "US" : entry.region == 4 ? "EU"
            : entry.region == 8                ? "KR"
                                               : nullptr;
        if (region)
        {
            const ImVec2 tag = TextSize(fonts.caption, region);
            const ImVec2 tag_at{after, middle - Dp(9)};
            draw->AddRectFilled(tag_at, {tag_at.x + tag.x + Dp(12), tag_at.y + Dp(18)}, Col(color::border, 0.8f),
                                Dp(5));
            DrawLabel(draw, fonts.caption, {tag_at.x + Dp(6), tag_at.y + (Dp(18) - tag.y) * 0.5f}, Col(color::muted),
                      region);
        }
        draw->PopClipRect();
        const auto right = [&](float column_end, const std::string& text, ImFont* font) {
            const ImVec2 size = TextSize(font, text.c_str());
            DrawLabel(draw, font, {column_end - size.x, middle - size.y * 0.5f}, Col(color::text), text.c_str());
        };
        right(x[2] - Dp(10), entry.wins ? std::to_string(*entry.wins) : "-", fonts.body);
        right(x[3] - Dp(10), entry.losses ? std::to_string(*entry.losses) : "-", fonts.body);
        right(at.x + width - Dp(8), std::to_string(entry.points), fonts.label);
        ImGui::Dummy({width, height});
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
