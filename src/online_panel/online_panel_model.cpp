#include "online_panel/online_panel_model.h"

#include "platform/competitive_rules.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace mscharged::online_panel
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
OnlinePanelRow PlayerRow(const ProfilePlayer& player, std::int64_t now)
{
    OnlinePanelRow row;
    row.kind = OnlinePanelRow::Kind::Player;
    row.player_id = player.id;
    row.key = "p" + std::to_string(player.id);
    row.name = player.name.empty() ? player.discord : player.name;
    row.image = player.avatar_url;
    row.initials = Initials(row.name);
    row.tint_seed = player.id;
    row.is_friend = player.is_friend;
    row.unread = player.unread;
    if (!player.online)
    {
        row.presence = Presence::Offline;
        row.status = platform::DescribeLastOnline(now, player.last_online);
        row.tone = Tone::Away;
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

// A player of the game's online mode, from DWC's status.
OnlinePanelRow MiiRow(const MiiProfile& mii, std::int64_t now)
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
    if (!mii.online)
    {
        row.presence = Presence::Offline;
        row.status = platform::DescribeLastOnline(now, mii.last_online);
        row.tone = Tone::Away;
    }
    else if (mii.status == "match")
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

// Online first (free, then busy, by name), then offline: the latest online first.
void SortRows(std::vector<OnlinePanelRow>& rows, const std::vector<std::optional<std::int64_t>>& last_online)
{
    std::vector<std::size_t> order(rows.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    const auto key = [&](std::size_t i) {
        const auto& row = rows[i];
        const bool offline = row.presence == Presence::Offline;
        const std::int64_t last = offline && i < last_online.size() && last_online[i] ? *last_online[i] : 0;
        return std::make_tuple(offline, row.presence == Presence::Busy, -last, Lower(row.name));
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
    std::vector<OnlinePanelRow> sorted;
    sorted.reserve(rows.size());
    for (const std::size_t i : order) sorted.push_back(std::move(rows[i]));
    rows = std::move(sorted);
}

std::size_t CountOnline(const std::vector<OnlinePanelRow>& rows)
{
    return std::size_t(std::count_if(rows.begin(), rows.end(),
                                     [](const OnlinePanelRow& row) { return row.presence != Presence::Offline; }));
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
    set.opponent = PlayerRow(match.opponent, context.now);
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
    if (row.kind != OnlinePanelRow::Kind::Player || row.presence == Presence::Offline) return;
    if (row.presence == Presence::Busy) return; // their own reason, already in `blocked`
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

    if (match) model.set = MakeSet(*match, context);
    const std::string blocker = ChallengeBlocker(match, context);
    if (context.in_match) model.note = "You're in a match: challenges wait until it ends.";
    else if (!context.can_challenge) model.note = "This server has no rules to play sets by yet.";

    // The friends, with the Discord friends who play Recharged.
    OnlinePanelSection friends;
    friends.id = OnlinePanelSection::Id::Friends;
    friends.title = "FRIENDS";
    friends.empty = "No friends yet. Add players with the heart, or on the launcher's Friends page.";
    std::vector<std::optional<std::int64_t>> last_online;
    std::set<std::int64_t> listed{me.id};
    std::set<std::int64_t> discord;
    for (const auto& player : context.discord_friends) discord.insert(player.id);
    for (const auto& player : lists.friends)
    {
        if (!listed.insert(player.id).second) continue;
        OnlinePanelRow row = PlayerRow(player, context.now);
        row.discord = discord.count(player.id) != 0;
        row.can_befriend = !row.discord;
        Challengeable(row, blocker);
        friends.rows.push_back(std::move(row));
        last_online.push_back(player.last_online);
    }
    for (const auto& player : context.discord_friends)
    {
        if (!listed.insert(player.id).second) continue;
        OnlinePanelRow row = PlayerRow(player, context.now);
        row.discord = true;
        Challengeable(row, blocker);
        friends.rows.push_back(std::move(row));
        last_online.push_back(player.last_online);
    }
    SortRows(friends.rows, last_online);
    friends.online = CountOnline(friends.rows);
    model.sections.push_back(std::move(friends));

    // While the player is available: the others available, then the game's
    // own online players by their Mii.
    std::set<std::int32_t> mii_friends;
    for (const auto& mii : lists.mii_friends) mii_friends.insert(mii.profile_id);
    if (me.available && context.random_games)
    {
        OnlinePanelSection available;
        available.id = OnlinePanelSection::Id::Available;
        available.title = "AVAILABLE NOW";
        available.empty = "Nobody else is available right now.";
        std::vector<std::optional<std::int64_t>> last;
        for (const auto& player : lists.available)
        {
            if (!listed.insert(player.id).second) continue;
            OnlinePanelRow row = PlayerRow(player, context.now);
            row.can_befriend = true;
            Challengeable(row, blocker);
            available.rows.push_back(std::move(row));
            last.push_back(player.last_online);
        }
        for (const auto& mii : lists.game_online)
        {
            if (mii_friends.count(mii.profile_id)) continue;
            available.rows.push_back(MiiRow(mii, context.now));
            last.push_back(mii.last_online);
        }
        SortRows(available.rows, last);
        available.online = CountOnline(available.rows);
        model.sections.push_back(std::move(available));
    }

    // The Mii friends (players of the game's online mode), when there are any.
    if (!lists.mii_friends.empty())
    {
        OnlinePanelSection miis;
        miis.id = OnlinePanelSection::Id::MiiFriends;
        miis.title = "MII FRIENDS";
        std::vector<std::optional<std::int64_t>> last;
        std::set<std::int32_t> seen;
        for (const auto& mii : lists.mii_friends)
        {
            if (!seen.insert(mii.profile_id).second) continue;
            miis.rows.push_back(MiiRow(mii, context.now));
            last.push_back(mii.last_online);
        }
        SortRows(miis.rows, last);
        miis.online = CountOnline(miis.rows);
        model.sections.push_back(std::move(miis));
    }
    return model;
}
} // namespace mscharged::online_panel
