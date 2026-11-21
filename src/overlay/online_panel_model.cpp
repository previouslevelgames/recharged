#include "overlay/online_panel_model.h"

#include "bootstrap/texts.h"
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
using platform::RuleSet;
using texts::T;

std::string Lower(std::string text)
{
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The rows say what a player is doing in the game's words: the mode first,
// then where in it ("Striker Cup · choosing a team").

// A match by its kind (the server's `match`); any other (an older game or
// server) just a match.
std::string MatchText(const std::string& kind)
{
    if (kind == "training") return T("menus.presence.match.training");
    if (kind == "ranked") return T("menus.presence.match.ranked");
    if (kind == "friends") return T("menus.presence.match.friends");
    if (kind == "tournament") return T("menus.presence.match.tournament");
    if (kind == "cpu") return T("menus.presence.match.cpu");
    if (kind == "local") return T("menus.presence.match.local");
    if (kind == "cup") return T("menus.presence.match.cup");
    if (kind == "challenge") return T("menus.presence.match.challenge");
    return T("menus.presence.match.other");
}

// The screens a player's game shows (the server's `screen`, ScreenOf), or
// nothing for one this build doesn't know.
std::string ScreenText(const std::string& screen)
{
    if (screen == "starting") return T("menus.presence.screen.starting");
    if (screen == "menus") return T("menus.presence.screen.menus");
    if (screen == "domination") return T("menus.presence.screen.domination");
    if (screen == "cup") return T("menus.presence.screen.cup");
    if (screen == "cup_team") return T("menus.presence.screen.cup_team");
    if (screen == "challenges") return T("menus.presence.screen.challenges");
    if (screen == "lessons") return T("menus.presence.screen.lessons");
    if (screen == "results") return T("menus.presence.screen.results");
    if (screen == "online") return T("menus.presence.screen.online");
    if (screen == "ranked") return T("menus.presence.screen.ranked");
    if (screen == "room") return T("menus.presence.screen.room");
    if (screen == "draft") return T("menus.presence.screen.draft");
    return {};
}

// Where a set is, as its player's row says it: the challenge, then the set
// against its opponent ("Set vs Mario · game 2 of 3").
std::string SetText(const platform::ListedSet& set, const ProfilePlayer& player)
{
    const texts::Arg name{"name", set.opponent};
    if (set.stage == "invited")
        return set.challenger ? T("menus.presence.set.waiting_answer", {name})
                              : T("menus.presence.set.challenged_by", {name});
    if (set.stage == "open") return T("menus.presence.set.getting_ready", {name});
    // Both are ready: the games start, sign in and look for each other.
    if (set.stage != "playing") return T("menus.presence.set.connecting", {name});
    if (player.activity == "match")
        return set.game > 0 && set.games > 1
            ? T("menus.presence.set.game", {name, {"game", std::to_string(set.game)},
                                            {"games", std::to_string(set.games)}})
            : T("menus.presence.set.playing", {name});
    if (player.screen == "draft") return T("menus.presence.set.choosing_teams", {name});
    return set.game > 0 ? T("menus.presence.set.between_games", {name}) : T("menus.presence.set.connecting", {name});
}

// What a Recharged player is doing, as the launcher's lists say it.
OnlinePanelRow PlayerRow(const ProfilePlayer& player)
{
    OnlinePanelRow row;
    row.kind = OnlinePanelRow::Kind::Player;
    row.player_id = player.id;
    row.key = "p" + std::to_string(player.id);
    row.name = player.name.empty() ? player.discord : player.name;
    row.image = PictureKey(player.avatar_url, player.mii_data);
    row.initials = Initials(row.name);
    row.tint_seed = player.id;
    row.country = player.country;
    if (!player.discord.empty()) row.detail = "@" + player.discord;
    row.is_friend = player.is_friend;
    const auto busy = [&](std::string status, const char* why) {
        row.presence = Presence::Busy;
        row.status = std::move(status);
        row.blocked = why;
        row.tone = Tone::Busy;
    };
    if (!player.online)
    {
        row.presence = Presence::Offline;
        row.status = T("menus.presence.offline");
        row.tone = Tone::Away;
        row.blocked = T("menus.blocked.offline");
        return row;
    }
    // A set that stopped before its games met (the server's `open`) keeps
    // nobody from a challenge, as the server has it (`busy`).
    if (player.set && player.busy) busy(SetText(*player.set, player), T("menus.blocked.in_set"));
    else if (player.busy) busy(T("menus.presence.in_set"), T("menus.blocked.in_set")); // a server before 1.2.12
    else if (player.activity == "match") busy(MatchText(player.match_kind), T("menus.blocked.in_match"));
    else if (player.screen == "draft") busy(ScreenText(player.screen), T("menus.blocked.in_online_match"));
    else
    {
        row.presence = Presence::Free;
        row.tone = Tone::Good;
        if (player.set) row.status = SetText(*player.set, player);
        else if (player.searching) row.status = T("menus.presence.searching");
        else if (const std::string screen = ScreenText(player.screen); !screen.empty()) row.status = screen;
        else
            row.status = player.activity == "game" ? T("menus.presence.in_game")
                : player.activity == "launcher"   ? T("menus.presence.in_launcher")
                                                  : T("menus.presence.online");
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
    row.image = MiiImageKey(mii.mii_data);
    row.initials = Initials(row.name);
    row.tint_seed = mii.profile_id;
    row.detail = mii.friend_code;
    row.is_friend = mii.is_friend;
    row.can_befriend = true;
    if (!mii.online)
    {
        row.presence = Presence::Offline;
        row.status = T("menus.presence.offline");
        row.tone = Tone::Away;
    }
    else if (mii.status == "match")
    {
        row.presence = Presence::Busy;
        row.status = MatchText(mii.match_kind);
        row.tone = Tone::Busy;
    }
    else
    {
        row.presence = Presence::Free;
        row.status = mii.status != "matchmaking" ? ScreenText("online")
            : mii.match_kind == "ranked"        ? ScreenText("ranked")
            : mii.match_kind == "friends"       ? ScreenText("room")
                                                : std::string(T("menus.presence.online_searching"));
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
    const bool locked = context.update_lock != UpdateLock::None;
    OnlinePanelSet set;
    set.match = match.id;
    set.opponent = PlayerRow(match.opponent);
    if (match.opponent_online && set.opponent.presence == Presence::Offline)
    {
        // The set knows they are online even when its copy of them is short.
        set.opponent.presence = Presence::Free;
        set.opponent.status = T("menus.presence.online");
        set.opponent.tone = Tone::Good;
    }
    const texts::Arg opponent{"name", set.opponent.name};
    const std::string rules = platform::DescribeRules(match.rules);
    set.rules = match.rule_set.empty() ? rules
        : rules.empty()                ? match.rule_set
                                       : T("menus.sets.rules", {{"rule_set", match.rule_set}, {"rules", rules}});
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
            set.title = T("menus.sets.challenges_you", {opponent});
            set.status = context.in_match ? std::string(T("menus.sets.answer_later"))
                : locked                  ? UpdateLockReason(context.update_lock)
                                          : std::string(T("menus.sets.accept_to_start"));
            set.tone = Tone::Neutral;
            set.can_accept = !context.in_match && !locked;
            set.can_decline = true;
        }
        else
        {
            set.stage = OnlinePanelSet::Stage::Waiting;
            set.title = T("menus.sets.your_challenge", {opponent});
            set.status = T("menus.sets.waiting_accept", {opponent});
            set.tone = Tone::Busy;
            set.can_cancel = true;
            set.cancel_label = T("menus.sets.withdraw");
        }
        return set;
    }
    if (match.state == "accepted")
    {
        set.title = T("menus.sets.your_set", {opponent});
        set.can_cancel = true;
        set.cancel_label = T("menus.sets.cancel_set");
        if (match.stage == "open")
        {
            set.stage = OnlinePanelSet::Stage::Ready;
            set.games = 0;
            if (!match.opponent_online)
            {
                set.status = T("menus.sets.closed_recharged", {opponent});
                set.tone = Tone::Bad;
            }
            else if (match.you_ready && match.opponent_ready)
            {
                set.status = T("menus.sets.both_ready");
                set.tone = Tone::Good;
            }
            else if (match.you_ready)
            {
                set.status = T("menus.sets.waiting_ready", {opponent});
                set.tone = Tone::Busy;
            }
            else
            {
                set.status = locked ? UpdateLockReason(context.update_lock)
                    : match.opponent_ready ? T("menus.sets.opponent_ready", {opponent})
                                           : std::string(T("menus.sets.ready_when_you_are"));
                set.tone = match.opponent_ready && !locked ? Tone::Good : Tone::Neutral;
                set.can_ready = !context.in_match && !locked;
            }
        }
        else if (match.stage == "go")
        {
            set.stage = OnlinePanelSet::Stage::Starting;
            set.games = 0;
            set.status = T("menus.sets.starting");
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
                set.status = T("menus.sets.play_ranked");
                set.tone = Tone::Neutral;
                set.cancel_label = T("menus.sets.cancel");
            }
            else if (match.reported)
            {
                set.status = T("menus.sets.waiting_result", {opponent});
                set.tone = Tone::Busy;
                set.can_cancel = false;
            }
            else
            {
                const int game = set.games > 0 ? std::min(set.won + set.lost + 1, set.games) : set.won + set.lost + 1;
                set.status = set.games > 0
                    ? texts::Count("menus.sets.game_best_of", set.games, {{"game", std::to_string(game)}})
                    : T("menus.sets.game", {{"game", std::to_string(game)}});
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
            set.title = T("menus.sets.results_differ");
            set.status = T("menus.sets.results_differ_about");
            set.tone = Tone::Bad;
        }
        else
        {
            set.title = match.winner == "you" ? T("menus.sets.won")
                : match.winner == "opponent"  ? T("menus.sets.lost")
                                              : T("menus.sets.draw");
            set.status = T("menus.sets.against", {opponent});
            set.tone = match.winner == "you" ? Tone::Good : match.winner == "opponent" ? Tone::Bad : Tone::Neutral;
        }
        set.can_rematch = sets && match.opponent_online && !context.in_match && !locked;
        return set;
    }
    // Declined, cancelled, expired or dissolved: nothing to show.
    return {};
}

// Whether the player can challenge anyone from this game now, and if not why.
std::string ChallengeBlocker(const std::optional<ProfileMatch>& match, const OnlinePanelContext& context)
{
    if (context.in_match) return T("menus.blocked.you_in_match");
    if (context.update_lock != UpdateLock::None) return UpdateLockReason(context.update_lock);
    if (match && match->Busy()) return T("menus.blocked.finish_set");
    if (!context.can_challenge) return T("menus.blocked.no_rules");
    return {};
}

// A game id's version as the panel tags it ("EU").
std::string VersionTag(const std::string& game)
{
    const char region = game.size() >= 4 ? game[3] : '?';
    return region == 'E' ? "US" : region == 'P' ? "EU" : region == 'J' ? "JP" : region == 'K' ? "KR" : game;
}

// Whether `player`'s row can be challenged to a set from this game: the
// player is free and nothing keeps this game from a set (`blocker`). A
// player of another version is tagged with it and can be challenged too: a
// set between two versions plays by the European one.
// A Mii whose profile is a Recharged player's is challenged as that player,
// while their Recharged is open and outside a match; else greyed, and why.
void ChallengeableMii(OnlinePanelRow& row, const MiiProfile& mii, const std::string& blocker)
{
    if (!mii.player_id)
    {
        row.blocked = T("menus.blocked.not_recharged");
        return;
    }
    row.player_id = mii.player_id;
    if (mii.player_activity.empty()) row.blocked = T("menus.blocked.recharged_closed");
    else if (mii.player_activity == "match") row.blocked = T("menus.blocked.in_match");
    else if (!blocker.empty()) row.blocked = blocker;
    else row.can_challenge = true;
}

void Challengeable(OnlinePanelRow& row, const ProfilePlayer& player, const std::string& blocker,
                   const OnlinePanelContext& context)
{
    if (row.kind != OnlinePanelRow::Kind::Player) return;
    const bool other = player.game.size() >= 4 && context.game.size() >= 4
        && player.game.compare(0, 4, context.game, 0, 4) != 0;
    if (other) row.version = VersionTag(player.game);
    if (row.presence != Presence::Free) return;
    if (!blocker.empty()) row.blocked = blocker;
    else row.can_challenge = true;
}
} // namespace

std::string UpdateLockReason(UpdateLock lock)
{
    using Lock = UpdateLock;
    return lock == Lock::Checking  ? T("menus.blocked.update_checking")
        : lock == Lock::Outdated   ? T("menus.blocked.update_outdated")
        : lock == Lock::Unchecked  ? T("menus.blocked.update_unchecked")
                                   : "";
}

std::string UpdateLockNote(UpdateLock lock)
{
    using Lock = UpdateLock;
    return lock == Lock::Checking  ? T("menus.players.update_checking")
        : lock == Lock::Outdated   ? T("menus.players.update_outdated")
        : lock == Lock::Unchecked  ? T("menus.players.update_unchecked")
                                   : "";
}

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
    model.update_lock = context.update_lock;
    if (context.signed_out)
    {
        model.signed_out = true;
        return model;
    }
    const ProfilePlayer& me = context.me;
    model.me.name = me.name.empty() ? me.discord : me.name;
    if (!me.discord.empty()) model.me.detail = "@" + me.discord;
    model.me.image = PictureKey(me.avatar_url, me.mii_data);
    model.me.initials = Initials(model.me.name);
    model.me.country = me.country;
    model.me.tint_seed = me.id;
    model.me.available = me.available;
    model.me.can_set_available = context.random_games;
    // Only where the server has rule sets to play by: the built-in presets,
    // the server's other rule sets, the player's own presets.
    if (!context.rule_sets.empty())
    {
        for (OnlinePanelPreset preset : context.built_in_presets)
        {
            for (const RuleSet& rule_set : context.rule_sets)
                if (rule_set.name == preset.name && rule_set.rules == preset.rules) preset.rule_set = rule_set.id;
            model.presets.push_back(std::move(preset));
        }
        for (const RuleSet& rule_set : context.rule_sets)
            if (std::none_of(context.built_in_presets.begin(), context.built_in_presets.end(),
                             [&](const OnlinePanelPreset& preset) { return preset.name == rule_set.name; }))
                model.presets.push_back({rule_set.name, rule_set.rules, rule_set.id});
        model.presets.insert(model.presets.end(), context.my_presets.begin(), context.my_presets.end());
    }
    for (std::size_t n = 0; n < model.presets.size(); ++n)
        if (model.presets[n].name == context.chosen_rules)
        {
            model.chosen_preset = int(n);
            break;
        }
    model.message = context.message;
    model.message_tone = context.error ? Tone::Bad : Tone::Neutral;
    if (context.search)
    {
        model.search.active = true;
        model.search.rules = context.search->rule_set;
        model.search.seconds = std::max(0, context.search_seconds);
        model.search.asked = context.search->asked;
    }
    model.can_search = context.searches && !context.search && !context.in_match
        && context.update_lock == UpdateLock::None && !model.presets.empty()
        && !(match && match->Busy());

    if (match) model.set = MakeSet(*match, context);
    const std::string blocker = ChallengeBlocker(match, context);
    if (context.in_match) model.note = T("menus.players.in_match");
    else if (context.update_lock != UpdateLock::None)
        model.note = UpdateLockNote(context.update_lock);
    else if (!context.can_challenge) model.note = T("menus.players.no_rules");

    // Online now: the friends with Recharged open, the Discord friends who
    // play it, and the Mii friends in the game's online mode.
    OnlinePanelSection online;
    online.id = OnlinePanelSection::Id::Online;
    online.title = T("menus.players.online_now");
    online.empty = context.loading ? T("menus.common.loading") : T("menus.players.no_friends_online");
    std::set<std::int64_t> listed{me.id};
    // The others are offline: each player once (as the lists below take
    // them), the player not among them.
    if (!context.loading)
    {
        std::set<std::int64_t> seen{me.id};
        for (const auto* list : {&lists.friends, &context.discord_friends})
            for (const auto& player : *list)
                if (seen.insert(player.id).second && !player.online) ++model.friends_offline;
        if (model.friends_offline > 0)
            online.aside = texts::Count("menus.players.friends_offline", model.friends_offline);
    }
    std::set<std::int64_t> discord;
    for (const auto& player : context.discord_friends) discord.insert(player.id);
    for (const auto& player : lists.friends)
    {
        if (!listed.insert(player.id).second || !player.online) continue;
        OnlinePanelRow row = PlayerRow(player);
        row.discord = discord.count(player.id) != 0;
        row.can_befriend = !row.discord;
        Challengeable(row, player, blocker, context);
        online.rows.push_back(std::move(row));
    }
    for (const auto& player : context.discord_friends)
    {
        if (!listed.insert(player.id).second || !player.online) continue;
        OnlinePanelRow row = PlayerRow(player);
        row.discord = true;
        Challengeable(row, player, blocker, context);
        online.rows.push_back(std::move(row));
    }
    std::set<std::int32_t> miis;
    for (const auto& mii : lists.mii_friends)
    {
        if (!miis.insert(mii.profile_id).second || !mii.online) continue;
        if (mii.player_id && !listed.insert(mii.player_id).second) continue;
        OnlinePanelRow row = MiiRow(mii);
        ChallengeableMii(row, mii, blocker);
        online.rows.push_back(std::move(row));
    }
    SortRows(online.rows);
    model.sections.push_back(std::move(online));

    // All of the friends, for the player's profile: online first (free, then
    // busy), then offline, each by name. Only friends added in the game can
    // be taken off again: Discord friends stay Discord's.
    OnlinePanelSection friends;
    friends.id = OnlinePanelSection::Id::Friends;
    friends.title = T("menus.profile.friends");
    friends.empty = context.loading ? T("menus.common.loading") : T("menus.profile.no_friends");
    std::set<std::int64_t> befriended{me.id};
    for (const auto* list : {&lists.friends, &context.discord_friends})
        for (const auto& player : *list)
        {
            if (!befriended.insert(player.id).second) continue;
            OnlinePanelRow row = PlayerRow(player);
            row.discord = discord.count(player.id) != 0;
            row.can_befriend = !row.discord;
            Challengeable(row, player, blocker, context);
            friends.rows.push_back(std::move(row));
        }
    std::set<std::int32_t> miiFriends;
    for (const auto& mii : lists.mii_friends)
    {
        if (!miiFriends.insert(mii.profile_id).second) continue;
        if (mii.player_id && !befriended.insert(mii.player_id).second) continue;
        OnlinePanelRow row = MiiRow(mii);
        ChallengeableMii(row, mii, blocker);
        friends.rows.push_back(std::move(row));
    }
    std::stable_sort(friends.rows.begin(), friends.rows.end(), [](const OnlinePanelRow& a, const OnlinePanelRow& b) {
        return std::make_tuple(int(a.presence), Lower(a.name)) < std::make_tuple(int(b.presence), Lower(b.name));
    });
    model.sections.push_back(std::move(friends));

    // While the player is available: the others available, then the game's
    // own online players by their Mii.
    if (me.available && context.random_games)
    {
        OnlinePanelSection available;
        available.id = OnlinePanelSection::Id::Available;
        available.title = T("menus.players.open_to_challenges");
        available.empty = context.loading ? T("menus.common.loading") : T("menus.players.nobody_open");
        for (const auto& player : lists.available)
        {
            if (!player.online || !listed.insert(player.id).second) continue;
            OnlinePanelRow row = PlayerRow(player);
            row.can_befriend = true;
            Challengeable(row, player, blocker, context);
            available.rows.push_back(std::move(row));
        }
        for (const auto& mii : lists.game_online)
        {
            if (!mii.online || !miis.insert(mii.profile_id).second) continue;
            if (mii.player_id && !listed.insert(mii.player_id).second) continue;
            OnlinePanelRow row = MiiRow(mii);
            ChallengeableMii(row, mii, blocker);
            available.rows.push_back(std::move(row));
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
