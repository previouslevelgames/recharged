#include "overlay/online_panel_model.h"

#include "platform/competitive_rules.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace mscharged::overlay
{
namespace
{
using platform::MiiProfile;
using platform::ProfileMatch;
using platform::ProfilePlayer;

std::string Lower(std::string text)
{
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// What a Recharged player is doing, as the launcher's lists say it.
OnlinePanelRow PlayerRow(const ProfilePlayer& player)
{
    OnlinePanelRow row;
    row.kind = OnlinePanelRow::Kind::Player;
    row.player_id = player.id;
    row.key = "p" + std::to_string(player.id);
    row.name = player.name.empty() ? player.discord : player.name;
    row.image = player.avatar_url;
    row.initials = Initials(row.name);
    row.tint_seed = player.id;
    if (!player.discord.empty()) row.detail = "@" + player.discord;
    row.is_friend = player.is_friend;
    if (!player.online)
    {
        row.presence = Presence::Offline;
        row.status = "Offline";
        row.tone = Tone::Away;
        row.blocked = "Offline";
    }
    else if (player.busy)
    {
        row.presence = Presence::Busy;
        row.status = row.blocked = "In a set";
        row.tone = Tone::Busy;
    }
    else if (player.activity == "match")
    {
        row.presence = Presence::Busy;
        row.status = "Playing a match";
        row.blocked = "In a match";
        row.tone = Tone::Busy;
    }
    else
    {
        row.presence = Presence::Free;
        row.status = player.activity == "game" ? "In the game"
            : player.activity == "launcher"   ? "In the launcher"
                                              : "Online";
        row.tone = Tone::Good;
    }
    return row;
}

// A player of the game's online mode, from DWC's status. Sets are for
// Recharged players: a Mii is played in the game's own online menu.
OnlinePanelRow MiiRow(const MiiProfile& mii)
{
    OnlinePanelRow row;
    row.kind = OnlinePanelRow::Kind::Mii;
    row.mii_profile = mii.profile_id;
    row.key = "m" + std::to_string(mii.profile_id);
    row.name = mii.mii.empty() ? std::string("Mii") : mii.mii;
    row.image = "mii:" + std::to_string(mii.profile_id);
    row.initials = Initials(row.name);
    row.tint_seed = mii.profile_id;
    row.detail = mii.friend_code;
    row.is_friend = mii.is_friend;
    row.can_befriend = true;
    row.blocked = "Play them in the game's online menu";
    if (mii.status == "match")
    {
        row.presence = Presence::Busy;
        row.status = "Playing a match";
        row.tone = Tone::Busy;
    }
    else
    {
        row.presence = Presence::Free;
        row.status = mii.status == "matchmaking" ? "Looking for a match" : "In the online menu";
        row.tone = Tone::Good;
    }
    return row;
}

// Free first, then busy, by name.
void SortRows(std::vector<OnlinePanelRow>& rows)
{
    std::stable_sort(rows.begin(), rows.end(), [](const OnlinePanelRow& a, const OnlinePanelRow& b) {
        return std::make_tuple(a.presence == Presence::Busy, Lower(a.name))
            < std::make_tuple(b.presence == Presence::Busy, Lower(b.name));
    });
}

// The games this player won and lost in the server's report of a set.
OnlinePanelContext::Score Reported(const ProfileMatch& match)
{
    OnlinePanelContext::Score score;
    for (const auto& game : match.games)
    {
        if (game.my_score > game.opponent_score) ++score.won;
        else if (game.my_score < game.opponent_score) ++score.lost;
    }
    return score;
}

OnlinePanelSet MakeSet(const ProfileMatch& match, const OnlinePanelContext& context)
{
    OnlinePanelSet set;
    set.match = match.id;
    set.opponent = PlayerRow(match.opponent);
    if (match.opponent_online && set.opponent.presence == Presence::Offline)
    {
        // The set knows they are online even when its copy of them is short.
        set.opponent.presence = Presence::Free;
        set.opponent.status = "Online";
        set.opponent.tone = Tone::Good;
    }
    const std::string& opponent = set.opponent.name;
    const std::string rules = platform::DescribeRules(match.rules);
    set.rules = match.rule_set.empty() ? rules : rules.empty() ? match.rule_set : match.rule_set + ": " + rules;
    set.games = match.rules.games;
    const OnlinePanelContext::Score reported = Reported(match);
    set.won = reported.won;
    set.lost = reported.lost;
    const bool sets = !match.stage.empty(); // a server with sets
    if (match.state == "challenged")
    {
        set.games = 0; // no score before it starts
        if (match.side == "challenged")
        {
            set.stage = OnlinePanelSet::Stage::Challenged;
            set.title = opponent + " challenges you";
            set.status = context.in_match ? "Answer once your match ends." : "Accept, and the set starts.";
            set.tone = Tone::Neutral;
            set.can_accept = !context.in_match;
            set.can_decline = true;
        }
        else
        {
            set.stage = OnlinePanelSet::Stage::Waiting;
            set.title = "Your challenge to " + opponent;
            set.status = "Waiting for " + opponent + " to accept";
            set.tone = Tone::Busy;
            set.can_cancel = true;
            set.cancel_label = "Withdraw";
        }
        return set;
    }
    if (match.state == "accepted")
    {
        set.title = "Your set against " + opponent;
        set.can_cancel = true;
        set.cancel_label = "Cancel set";
        if (match.stage == "open")
        {
            set.stage = OnlinePanelSet::Stage::Ready;
            set.games = 0;
            if (!match.opponent_online)
            {
                set.status = opponent + " has closed Recharged";
                set.tone = Tone::Bad;
            }
            else if (match.you_ready && match.opponent_ready)
            {
                set.status = "Both are ready";
                set.tone = Tone::Good;
            }
            else if (match.you_ready)
            {
                set.status = "Waiting for " + opponent + " to be ready";
                set.tone = Tone::Busy;
            }
            else
            {
                set.status = match.opponent_ready ? opponent + " is ready" : "Ready when you are";
                set.tone = match.opponent_ready ? Tone::Good : Tone::Neutral;
                set.can_ready = !context.in_match;
            }
        }
        else if (match.stage == "go")
        {
            set.stage = OnlinePanelSet::Stage::Starting;
            set.games = 0;
            set.status = "Both are ready: the set starts";
            set.tone = Tone::Good;
        }
        else
        {
            set.stage = OnlinePanelSet::Stage::Playing;
            if (context.score)
            {
                set.won = context.score->won;
                set.lost = context.score->lost;
            }
            if (!sets)
            {
                // A server before sets: both play RANKED themselves.
                set.games = 0;
                set.status = "Play RANKED in the game's online menu";
                set.tone = Tone::Neutral;
                set.cancel_label = "Cancel";
            }
            else if (match.reported)
            {
                set.status = "Waiting for " + opponent + "'s result";
                set.tone = Tone::Busy;
                set.can_cancel = false;
            }
            else
            {
                const int game = set.games > 0 ? std::min(set.won + set.lost + 1, set.games) : set.won + set.lost + 1;
                set.status = "Game " + std::to_string(game)
                    + (set.games > 0 ? " · best of " + std::to_string(set.games) : std::string());
                set.tone = Tone::Good;
                set.can_cancel = false;
            }
        }
        return set;
    }
    if (match.state == "finished" || match.state == "disputed")
    {
        set.stage = OnlinePanelSet::Stage::Ended;
        if (match.state == "disputed")
        {
            set.title = "The results differ";
            set.status = "The two games saw different results; nothing was counted.";
            set.tone = Tone::Bad;
        }
        else
        {
            set.title = match.winner == "you" ? "You won the set"
                : match.winner == "opponent"  ? "You lost the set"
                                              : "The set is a draw";
            set.status = "Against " + opponent;
            set.tone = match.winner == "you" ? Tone::Good : match.winner == "opponent" ? Tone::Bad : Tone::Neutral;
        }
        set.can_rematch = sets && match.opponent_online && !context.in_match;
        return set;
    }
    // Declined, cancelled, expired or dissolved: nothing to show.
    return {};
}

// Whether the player can challenge anyone from this game now, and if not why.
std::string ChallengeBlocker(const std::optional<ProfileMatch>& match, const OnlinePanelContext& context)
{
    if (context.in_match) return "You're in a match";
    if (match && match->Open()) return "Finish your set first";
    if (!context.can_challenge) return "No rules to play by";
    return {};
}

void Challengeable(OnlinePanelRow& row, const std::string& blocker)
{
    if (row.kind != OnlinePanelRow::Kind::Player || row.presence != Presence::Free) return;
    if (!blocker.empty()) row.blocked = blocker;
    else row.can_challenge = true;
}
} // namespace

std::string Initials(const std::string& name)
{
    if (name.empty()) return "?";
    const auto byte = static_cast<unsigned char>(name[0]);
    if (byte >= 0x80)
        return name.substr(0, std::min<std::size_t>(byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2, name.size()));
    return std::string(1, char(std::toupper(byte)));
}

OnlinePanelModel MakeOnlinePanelModel(const platform::PlayerLists& lists,
                                      const std::optional<platform::ProfileMatch>& match,
                                      const OnlinePanelContext& context)
{
    OnlinePanelModel model;
    const ProfilePlayer& me = context.me;
    model.me.name = me.name.empty() ? me.discord : me.name;
    if (!me.discord.empty()) model.me.detail = "@" + me.discord;
    model.me.image = me.avatar_url;
    model.me.initials = Initials(model.me.name);
    model.me.tint_seed = me.id;
    model.me.available = me.available;
    model.me.can_set_available = context.random_games;
    model.challenge_rules = context.challenge_rules;
    model.message = context.message;
    model.message_tone = context.error ? Tone::Bad : Tone::Neutral;

    if (match) model.set = MakeSet(*match, context);
    const std::string blocker = ChallengeBlocker(match, context);
    if (context.in_match) model.note = "You're in a match: challenges wait until it ends.";
    else if (!context.can_challenge) model.note = "This server has no rules to play sets by yet.";

    // Online now: the friends with Recharged open, the Discord friends who
    // play it, and the Mii friends in the game's online mode.
    OnlinePanelSection online;
    online.id = OnlinePanelSection::Id::Online;
    online.title = "ONLINE NOW";
    online.empty = context.loading ? "Loading..." : "None of your friends is online";
    if (!context.loading)
        online.hint = "See your offline friends and find new ones on the launcher's Online pages.";
    std::set<std::int64_t> listed{me.id};
    std::set<std::int64_t> discord;
    for (const auto& player : context.discord_friends) discord.insert(player.id);
    for (const auto& player : lists.friends)
    {
        if (!listed.insert(player.id).second || !player.online) continue;
        OnlinePanelRow row = PlayerRow(player);
        row.discord = discord.count(player.id) != 0;
        row.can_befriend = !row.discord;
        Challengeable(row, blocker);
        online.rows.push_back(std::move(row));
    }
    for (const auto& player : context.discord_friends)
    {
        if (!listed.insert(player.id).second || !player.online) continue;
        OnlinePanelRow row = PlayerRow(player);
        row.discord = true;
        Challengeable(row, blocker);
        online.rows.push_back(std::move(row));
    }
    std::set<std::int32_t> miis;
    for (const auto& mii : lists.mii_friends)
    {
        if (!miis.insert(mii.profile_id).second || !mii.online) continue;
        online.rows.push_back(MiiRow(mii));
    }
    SortRows(online.rows);
    model.sections.push_back(std::move(online));

    // While the player is available: the others available, then the game's
    // own online players by their Mii.
    if (me.available && context.random_games)
    {
        OnlinePanelSection available;
        available.id = OnlinePanelSection::Id::Available;
        available.title = "AVAILABLE NOW";
        available.empty = context.loading ? "Loading..." : "Nobody else is available right now";
        for (const auto& player : lists.available)
        {
            if (!player.online || !listed.insert(player.id).second) continue;
            OnlinePanelRow row = PlayerRow(player);
            row.can_befriend = true;
            Challengeable(row, blocker);
            available.rows.push_back(std::move(row));
        }
        for (const auto& mii : lists.game_online)
        {
            if (!mii.online || !miis.insert(mii.profile_id).second) continue;
            available.rows.push_back(MiiRow(mii));
        }
        SortRows(available.rows);
        model.sections.push_back(std::move(available));
    }
    return model;
}

int OnlinePanelWaiting(const OnlinePanelModel& model)
{
    const OnlinePanelSet& set = model.set;
    if (set.stage == OnlinePanelSet::Stage::Challenged && (set.can_accept || set.can_decline)) return 1;
    if (set.stage == OnlinePanelSet::Stage::Ready && set.can_ready) return 1;
    return 0;
}

const OnlinePanelRow* FindRow(const OnlinePanelModel& model, const std::string& key)
{
    for (const auto& section : model.sections)
        for (const auto& row : section.rows)
            if (row.key == key) return &row;
    if (model.set.stage != OnlinePanelSet::Stage::None && model.set.opponent.key == key) return &model.set.opponent;
    return nullptr;
}
} // namespace mscharged::overlay
