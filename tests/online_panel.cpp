// The overlay's Online tab data (src/overlay/online_panel_model.cpp) from
// made-up lists and sets: who is online now and in which order, who is
// available, what their rows say, who can be challenged, and the set at each
// of its steps, what waits for the player (the tab's badge), and the rule sets
// the player's challenges can play by. Offline friends are counted, never
// listed.

#include "overlay/online_panel_model.h"
#include "platform/competitive_rules.h"
#include "platform/match_kind.h"

#include <cstdio>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>

namespace platform = mscharged::platform;
using namespace mscharged::overlay;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (ok) return;
    std::fprintf(stderr, "online_panel.cpp:%d: %s\n", line, what);
    ++failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

platform::ProfilePlayer Player(std::int64_t id, const char* name)
{
    platform::ProfilePlayer player;
    player.id = id;
    player.name = name;
    player.discord = name;
    return player;
}

platform::ProfilePlayer Online(std::int64_t id, const char* name, const char* activity, bool busy = false)
{
    auto player = Player(id, name);
    player.online = true;
    player.activity = activity;
    player.busy = busy;
    return player;
}

platform::MiiProfile Mii(std::int32_t id, const char* name, bool online, const char* status, bool is_friend)
{
    platform::MiiProfile mii;
    mii.profile_id = id;
    mii.mii = name;
    mii.friend_code = "0868-9934-5920";
    mii.online = online;
    mii.status = status;
    mii.is_friend = is_friend;
    return mii;
}

OnlinePanelContext Context()
{
    OnlinePanelContext context;
    context.me = Player(1, "Anosch");
    context.me.discord = "anosch";
    context.me.avatar_url = "https://cdn.example/a.png";
    return context;
}

const OnlinePanelSection* Section(const OnlinePanelModel& model, OnlinePanelSection::Id id)
{
    for (const auto& section : model.sections)
        if (section.id == id) return &section;
    return nullptr;
}

const OnlinePanelRow* Row(const OnlinePanelSection* section, const std::string& name)
{
    if (!section) return nullptr;
    for (const auto& row : section->rows)
        if (row.name == name) return &row;
    return nullptr;
}

platform::ProfileMatch Set(const char* state, const char* side, const char* stage)
{
    platform::ProfileMatch set;
    set.id = 77;
    set.state = state;
    set.side = side;
    set.stage = stage;
    set.opponent = Online(10, "Mario", "launcher");
    set.opponent_online = true;
    set.rule_set = "Competitive";
    set.rules.skill = "rookie";
    set.rules.limit = "time";
    set.rules.time = 180;
    set.rules.games = 3;
    set.rules.stadium = "random";
    return set;
}

void TestPlayer()
{
    auto context = Context();
    context.message = "The server can't be reached.";
    context.error = true;
    const auto model = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(model.me.name == "Anosch");
    CHECK(model.me.detail == "@anosch");
    CHECK(model.me.image == "https://cdn.example/a.png");
    CHECK(model.me.initials == "A");
    CHECK(!model.me.available);
    CHECK(model.me.can_set_available);
    CHECK(model.set.stage == OnlinePanelSet::Stage::None);
    CHECK(model.note.empty());
    CHECK(model.message == "The server can't be reached." && model.message_tone == Tone::Bad);
    // Not available: Online now alone, which says nobody is online (and,
    // without friends, nothing about friends offline).
    CHECK(model.sections.size() == 1);
    CHECK(model.sections[0].id == OnlinePanelSection::Id::Online);
    CHECK(model.sections[0].rows.empty());
    CHECK(model.sections[0].empty == "None of your friends is online");
    CHECK(model.sections[0].hint.empty() && model.sections[0].aside.empty() && model.friends_offline == 0);
    CHECK(OnlinePanelWaiting(model) == 0);

    context.loading = true;
    const auto loading = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(loading.sections[0].empty == "Loading..." && loading.sections[0].hint.empty());

    context.loading = false;
    context.random_games = false;
    context.me.available = true;
    const auto old_server = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(!old_server.me.can_set_available);
    CHECK(!Section(old_server, OnlinePanelSection::Id::Available));

    CHECK(Initials("mario") == "M");
    CHECK(Initials("\xC3\xA9lodie") == "\xC3\xA9");
    CHECK(Initials("\xE2\x98\x85star") == "\xE2\x98\x85");
    CHECK(Initials("") == "?");
}

void TestOnlineNow()
{
    platform::PlayerLists lists;
    lists.friends = {Player(20, "Rosalina"), Online(10, "Mario", "launcher"), Online(11, "luigi", "game", true),
                     Online(12, "Daisy", "game"), Player(21, "Wario"), Online(13, "Toad", "match"),
                     Player(1, "Anosch")};
    for (auto& player : lists.friends) player.is_friend = true;
    lists.mii_friends = {Mii(502, "Sara", false, "", true), Mii(501, "Tommy", true, "matchmaking", true),
                         Mii(503, "Kai", true, "match", true)};
    auto context = Context();
    auto daisy = lists.friends[3];
    daisy.discord_id = "9001";
    context.discord_friends = {Player(31, "Bowser"), Online(30, "Peach", "game"), daisy, Player(20, "Rosalina")};
    const auto model = MakeOnlinePanelModel(lists, std::nullopt, context);
    const auto* online = Section(model, OnlinePanelSection::Id::Online);
    CHECK(online != nullptr);
    if (!online) return;
    // Only those online: free first by name (any case), then busy; Miis among them.
    const char* order[] = {"Daisy", "Mario", "Peach", "Tommy", "Kai", "luigi", "Toad"};
    CHECK(online->rows.size() == std::size(order));
    for (std::size_t i = 0; i < std::size(order) && i < online->rows.size(); ++i)
        if (online->rows[i].name != order[i])
            std::fprintf(stderr, "row %zu: %s, wanted %s\n", i, online->rows[i].name.c_str(), order[i]), ++failures;
    CHECK(!Row(online, "Rosalina") && !Row(online, "Wario") && !Row(online, "Bowser") && !Row(online, "Sara"));

    const auto* mario = Row(online, "Mario");
    CHECK(mario && mario->status == "In the launcher" && mario->tone == Tone::Good);
    CHECK(mario && mario->presence == Presence::Free && mario->can_challenge && mario->blocked.empty());
    CHECK(mario && mario->is_friend && mario->can_befriend && !mario->discord);
    CHECK(mario && mario->key == "p10" && mario->player_id == 10 && mario->detail == "@Mario");
    const auto* luigi = Row(online, "luigi");
    CHECK(luigi && luigi->status == "In a set" && luigi->presence == Presence::Busy && !luigi->can_challenge);
    CHECK(luigi && luigi->blocked == "In a set" && luigi->initials == "L");
    const auto* toad = Row(online, "Toad");
    CHECK(toad && toad->status == "Playing a match" && toad->blocked == "In a match" && !toad->can_challenge);
    // A friend who is a Discord friend too: once, with Discord's logo, no friend button.
    const auto* daisy_row = Row(online, "Daisy");
    CHECK(daisy_row && daisy_row->discord && daisy_row->is_friend && !daisy_row->can_befriend);
    CHECK(daisy_row && daisy_row->status == "In the game");
    // A Discord friend who isn't a friend here: listed, no friend button.
    const auto* peach = Row(online, "Peach");
    CHECK(peach && peach->discord && !peach->is_friend && !peach->can_befriend && peach->can_challenge);
    // Mii friends: the game's online mode, never a set.
    const auto* tommy = Row(online, "Tommy");
    CHECK(tommy && tommy->kind == OnlinePanelRow::Kind::Mii && tommy->mii_profile == 501 && tommy->key == "m501");
    CHECK(tommy && tommy->status == "Looking for a match" && tommy->image == "mii:501");
    CHECK(tommy && tommy->detail == "0868-9934-5920" && tommy->is_friend && tommy->can_befriend);
    CHECK(tommy && !tommy->can_challenge && !tommy->blocked.empty());
    const auto* kai = Row(online, "Kai");
    CHECK(kai && kai->presence == Presence::Busy && kai->status == "Playing a match");

    // A player's screen finds them by key, and nobody when they went offline.
    CHECK(FindRow(model, "p10") == mario);
    CHECK(FindRow(model, "m501") == tommy);
    CHECK(FindRow(model, "p20") == nullptr);

    // Offline: Rosalina (a Discord friend too: once), Wario and Bowser; not
    // the player, nor Sara (a Mii friend: the game's online mode).
    CHECK(model.friends_offline == 3);
    CHECK(online->aside == "3 friends offline");
    CHECK(online->hint.empty());
    // One alone; while the lists load, nothing.
    platform::PlayerLists one;
    one.friends = {Player(20, "Rosalina"), Online(10, "Mario", "launcher")};
    auto alone = MakeOnlinePanelModel(one, std::nullopt, Context());
    CHECK(alone.friends_offline == 1 && alone.sections[0].aside == "1 friend offline");
    auto loading = Context();
    loading.loading = true;
    alone = MakeOnlinePanelModel(one, std::nullopt, loading);
    CHECK(alone.friends_offline == 0 && alone.sections[0].aside.empty());
    // Everyone online: nothing.
    one.friends = {Online(10, "Mario", "launcher")};
    alone = MakeOnlinePanelModel(one, std::nullopt, Context());
    CHECK(alone.friends_offline == 0 && alone.sections[0].aside.empty());
}

void TestAvailable()
{
    platform::PlayerLists lists;
    lists.friends = {Online(10, "Mario", "launcher")};
    lists.friends[0].is_friend = true;
    lists.available = {Online(40, "Shy Guy", "launcher"), Online(10, "Mario", "launcher"),
                       Online(41, "Koopa", "match"), Player(42, "Gone")};
    lists.game_online = {Mii(500, "Kenji", true, "match", false), Mii(501, "Tommy", true, "menu", true)};
    lists.mii_friends = {Mii(501, "Tommy", true, "matchmaking", true)};
    auto context = Context();
    // Not available: no list of available players.
    auto model = MakeOnlinePanelModel(lists, std::nullopt, context);
    CHECK(!Section(model, OnlinePanelSection::Id::Available));

    context.me.available = true;
    model = MakeOnlinePanelModel(lists, std::nullopt, context);
    CHECK(model.me.available);
    CHECK(model.sections.size() == 2);
    const auto* available = Section(model, OnlinePanelSection::Id::Available);
    CHECK(available != nullptr);
    if (!available) return;
    // The friends and the Mii friends are in Online now already; nobody offline.
    CHECK(available->rows.size() == 3);
    CHECK(!Row(available, "Mario") && !Row(available, "Tommy") && !Row(available, "Gone"));
    const auto* shy = Row(available, "Shy Guy");
    CHECK(shy && shy->can_challenge && shy->can_befriend && !shy->is_friend);
    const auto* koopa = Row(available, "Koopa");
    CHECK(koopa && !koopa->can_challenge && koopa->blocked == "In a match");
    const auto* kenji = Row(available, "Kenji");
    CHECK(kenji && kenji->kind == OnlinePanelRow::Kind::Mii && kenji->status == "Playing a match");
    CHECK(kenji && kenji->presence == Presence::Busy && !kenji->is_friend && kenji->can_befriend);
    CHECK(available->rows[0].name == "Shy Guy");

    // Nobody: it says so.
    lists.available.clear();
    lists.game_online.clear();
    model = MakeOnlinePanelModel(lists, std::nullopt, context);
    available = Section(model, OnlinePanelSection::Id::Available);
    CHECK(available && available->rows.empty() && !available->empty.empty());
}

void TestChallengeBlockers()
{
    platform::PlayerLists lists;
    lists.friends = {Online(10, "Mario", "launcher")};
    auto context = Context();
    context.in_match = true;
    auto model = MakeOnlinePanelModel(lists, std::nullopt, context);
    const auto* mario = Row(Section(model, OnlinePanelSection::Id::Online), "Mario");
    CHECK(mario && !mario->can_challenge && mario->blocked == "You're in a match");
    CHECK(!model.note.empty());

    context.in_match = false;
    context.can_challenge = false;
    model = MakeOnlinePanelModel(lists, std::nullopt, context);
    mario = Row(Section(model, OnlinePanelSection::Id::Online), "Mario");
    CHECK(mario && !mario->can_challenge && !mario->blocked.empty());
    CHECK(!model.note.empty());

    // An open set: no other challenge. A set that ended: challenges again.
    context.can_challenge = true;
    model = MakeOnlinePanelModel(lists, Set("challenged", "challenger", "invited"), context);
    mario = Row(Section(model, OnlinePanelSection::Id::Online), "Mario");
    CHECK(mario && !mario->can_challenge && mario->blocked == "Finish your set first");
    CHECK(model.note.empty());
    model = MakeOnlinePanelModel(lists, Set("finished", "challenger", "finished"), context);
    mario = Row(Section(model, OnlinePanelSection::Id::Online), "Mario");
    CHECK(mario && mario->can_challenge);
    // A stopped set (a player left before the games met) keeps nobody from
    // another one; one that goes does.
    model = MakeOnlinePanelModel(lists, Set("accepted", "challenger", "open"), context);
    mario = Row(Section(model, OnlinePanelSection::Id::Online), "Mario");
    CHECK(mario && mario->can_challenge);
    model = MakeOnlinePanelModel(lists, Set("accepted", "challenger", "go"), context);
    mario = Row(Section(model, OnlinePanelSection::Id::Online), "Mario");
    CHECK(mario && !mario->can_challenge && mario->blocked == "Finish your set first");
}

void TestVersions()
{
    // This game is the US one: a player of the European one is tagged and
    // can't be challenged (RANKED never pairs the two); a US one can; one
    // whose Recharged doesn't say can, as before.
    platform::PlayerLists lists;
    auto eu = Online(10, "Birnen", "game");
    eu.game = "R4QP";
    auto us = Online(11, "Kiki", "game");
    us.game = "R4QE";
    lists.friends = {eu, us, Online(12, "Lou", "launcher")};
    auto context = Context();
    context.game = "R4QE";
    const auto model = MakeOnlinePanelModel(lists, std::nullopt, context);
    const auto* online = Section(model, OnlinePanelSection::Id::Online);
    const auto* birnen = Row(online, "Birnen");
    CHECK(birnen && birnen->version == "EU" && !birnen->can_challenge
          && birnen->blocked == "Plays the European version");
    const auto* kiki = Row(online, "Kiki");
    CHECK(kiki && kiki->version.empty() && kiki->can_challenge);
    const auto* lou = Row(online, "Lou");
    CHECK(lou && lou->version.empty() && lou->can_challenge);
}

void TestSet()
{
    using Stage = OnlinePanelSet::Stage;
    auto context = Context();
    // A challenge to the player.
    auto model = MakeOnlinePanelModel({}, Set("challenged", "challenged", "invited"), context);
    CHECK(model.set.stage == Stage::Challenged && model.set.match == 77);
    CHECK(OnlinePanelWaiting(model) == 1);
    CHECK(model.set.title == "Mario challenges you");
    CHECK(model.set.can_accept && model.set.can_decline && !model.set.can_cancel);
    CHECK(model.set.games == 0);
    CHECK(model.set.rules.rfind("Competitive: 3 minutes", 0) == 0);
    CHECK(model.set.opponent.name == "Mario" && model.set.opponent.presence == Presence::Free);
    CHECK(FindRow(model, "p10") == &model.set.opponent);
    context.in_match = true;
    model = MakeOnlinePanelModel({}, Set("challenged", "challenged", "invited"), context);
    CHECK(!model.set.can_accept && model.set.can_decline);
    CHECK(OnlinePanelWaiting(model) == 1); // to decline, or to accept after the match
    context.in_match = false;

    // The player's challenge.
    model = MakeOnlinePanelModel({}, Set("challenged", "challenger", "invited"), context);
    CHECK(model.set.stage == Stage::Waiting && model.set.status == "Waiting for Mario to accept");
    CHECK(OnlinePanelWaiting(model) == 0);
    CHECK(model.set.can_cancel && model.set.cancel_label == "Withdraw" && !model.set.can_accept);

    // Accepted: Ready to play!, then waiting for the opponent.
    auto set = Set("accepted", "challenger", "open");
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.stage == Stage::Ready && model.set.can_ready && model.set.status == "Ready when you are");
    CHECK(OnlinePanelWaiting(model) == 1);
    CHECK(model.set.can_cancel && model.set.cancel_label == "Cancel set");
    set.you_ready = true;
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(!model.set.can_ready && model.set.status == "Waiting for Mario to be ready");
    CHECK(OnlinePanelWaiting(model) == 0);
    set.opponent_online = false;
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.tone == Tone::Bad && model.set.status == "Mario has closed Recharged");

    // Playing: the game's count first, else the server's report.
    set = Set("accepted", "challenger", "playing");
    set.games = {{2, 1, "mario", "bowser", "the_palace"}};
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.stage == Stage::Playing && model.set.won == 1 && model.set.lost == 0 && model.set.games == 3);
    CHECK(model.set.status == "Game 2 · best of 3");
    CHECK(!model.set.can_cancel);
    context.score = OnlinePanelContext::Score{1, 1};
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.won == 1 && model.set.lost == 1 && model.set.status == "Game 3 · best of 3");
    context.score.reset();
    set.reported = true;
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.status == "Waiting for Mario's result");

    // A server before sets.
    set = Set("accepted", "challenger", "");
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.stage == Stage::Playing && model.set.games == 0 && model.set.can_cancel);

    // Ended: the result and the rematch while the opponent is online.
    set = Set("finished", "challenged", "finished");
    set.winner = "you";
    set.games = {{3, 1, "", "", ""}, {0, 2, "", "", ""}, {2, 1, "", "", ""}};
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.stage == Stage::Ended && model.set.title == "You won the set" && model.set.tone == Tone::Good);
    CHECK(model.set.won == 2 && model.set.lost == 1 && model.set.can_rematch);
    set.opponent_online = false;
    set.opponent.online = false;
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(!model.set.can_rematch);
    set.state = "disputed";
    model = MakeOnlinePanelModel({}, set, context);
    CHECK(model.set.stage == Stage::Ended && model.set.title == "The results differ" && model.set.tone == Tone::Bad);

    // Ended without a result: nothing.
    for (const char* state : {"declined", "cancelled", "expired"})
    {
        set.state = state;
        model = MakeOnlinePanelModel({}, set, context);
        CHECK(model.set.stage == Stage::None);
    }
}
// What a challenge starts from: the server's rule sets, then the player's
// own presets (only where the server has rule sets), the one chosen last (by
// name; else the first) for its setup.
void TestChallengeRules()
{
    auto context = Context();
    OnlinePanelPreset mine{"Palace tilt", {}, 0};
    mine.rules.stadium = "the_palace";
    context.my_presets = {mine};
    // Without rule sets: nothing to start from.
    auto model = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(model.presets.empty() && model.chosen_preset == 0);

    platform::RuleSet competitive;
    competitive.id = 2;
    competitive.name = "Competitive";
    competitive.rules.skill = "rookie";
    competitive.rules.limit = "time";
    competitive.rules.time = 180;
    competitive.rules.games = 3;
    competitive.rules.stadium = "random";
    competitive.rules.tweaks = {"no_mega_strikes"};
    platform::RuleSet classic = competitive;
    classic.id = 5;
    classic.name = "Classic";
    classic.rules.time = 300;
    classic.rules.games = 1;
    classic.rules.tweaks.clear();
    context.rule_sets = {competitive, classic};
    context.chosen_rules = "Classic";
    model = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(model.presets.size() == 3 && model.presets[0].name == "Competitive" && model.presets[0].rule_set == 2
          && model.presets[1].name == "Classic" && model.presets[1].rules == classic.rules
          && model.presets[2].name == "Palace tilt" && model.presets[2].rule_set == 0);
    CHECK(model.chosen_preset == 1);
    // The built-in presets first: by the server's rule set of the same name
    // when its rules are the same (Competitive, id 2), else as they are
    // (Retail ranked, none here); the server's Competitive doesn't show twice.
    {
        auto with = context;
        with.built_in_presets = {{"Retail ranked", classic.rules, 0}, {"Competitive", competitive.rules, 0}};
        const auto built = MakeOnlinePanelModel({}, std::nullopt, with);
        CHECK(built.presets.size() == 4 && built.presets[0].name == "Retail ranked" && built.presets[0].rule_set == 0
              && built.presets[1].name == "Competitive" && built.presets[1].rule_set == 2
              && built.presets[2].name == "Classic" && built.presets[3].name == "Palace tilt");
        auto other = competitive.rules;
        other.games = 7;
        with.built_in_presets = {{"Competitive", other, 0}};
        const auto changed = MakeOnlinePanelModel({}, std::nullopt, with);
        CHECK(changed.presets[0].name == "Competitive" && changed.presets[0].rule_set == 0
              && changed.presets[0].rules.games == 7 && changed.presets.size() == 3);
    }
    context.chosen_rules = "Palace tilt";
    CHECK(MakeOnlinePanelModel({}, std::nullopt, context).chosen_preset == 2);
    // None chosen, or one there isn't: the first.
    for (const char* name : {"", "Gone", "classic"})
    {
        context.chosen_rules = name;
        CHECK(MakeOnlinePanelModel({}, std::nullopt, context).chosen_preset == 0);
    }
}

// A search for a random opponent: possible with a server that knows them and
// rules to play by, not in a match or with a set to play; while it runs, how
// long and how many were asked.
void TestSearch()
{
    auto context = Context();
    platform::RuleSet competitive;
    competitive.id = 2;
    competitive.name = "Competitive";
    context.rule_sets = {competitive};
    CHECK(!MakeOnlinePanelModel({}, std::nullopt, context).can_search);
    context.searches = true;
    auto model = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(model.can_search && !model.search.active);
    context.in_match = true;
    CHECK(!MakeOnlinePanelModel({}, std::nullopt, context).can_search);
    context.in_match = false;
    platform::ProfileMatch set;
    set.id = 4;
    set.state = "accepted";
    CHECK(!MakeOnlinePanelModel({}, set, context).can_search);
    platform::ProfileSearch search;
    search.rule_set = "Palace";
    search.asked = 2;
    context.search = search;
    context.search_seconds = 75;
    model = MakeOnlinePanelModel({}, std::nullopt, context);
    CHECK(model.search.active && model.search.rules == "Palace" && model.search.seconds == 75
          && model.search.asked == 2 && !model.can_search);
}
} // namespace

// What a match is: its kind as the server says it; an older game or server
// says none.
void TestMatchKinds()
{
    platform::PlayerLists lists;
    const std::pair<const char*, const char*> kinds[] = {
        {"ranked", "Ranked match"},   {"friends", "Friend match"},
        {"tournament", "Online tournament"}, {"cpu", "Match vs CPU"},
        {"local", "Local match"},     {"cup", "Road to the Striker Cup"},
        {"challenge", "Striker Challenge"}, {"training", "Strikers 101"},
        {"", "Playing a match"},      {"golf", "Playing a match"},
    };
    std::int64_t id = 10;
    for (const auto& [kind, text] : kinds)
    {
        auto player = Online(id, (std::string("P") + std::to_string(id)).c_str(), "match");
        player.match_kind = kind;
        lists.friends.push_back(player);
        ++id;
    }
    // Miis: whom DWC's matchmaking was with.
    const std::tuple<const char*, const char*, const char*> miis[] = {
        {"match", "ranked", "Ranked match"},
        {"match", "friends", "Friend match"},
        {"match", "", "Playing a match"},
        {"matchmaking", "ranked", "Looking for a ranked match"},
        {"matchmaking", "friends", "In a friend room"},
        {"matchmaking", "", "Looking for a match"},
        {"menu", "", "In the online menu"},
    };
    std::int32_t profile = 600;
    for (const auto& [status, kind, text] : miis)
    {
        auto mii = Mii(profile, (std::string("M") + std::to_string(profile)).c_str(), true, status, true);
        mii.match_kind = kind;
        lists.mii_friends.push_back(mii);
        ++profile;
    }
    const auto model = MakeOnlinePanelModel(lists, std::nullopt, Context());
    const auto* online = Section(model, OnlinePanelSection::Id::Online);
    CHECK(online != nullptr);
    if (!online) return;
    id = 10;
    for (const auto& [kind, text] : kinds)
    {
        const auto* row = Row(online, (std::string("P") + std::to_string(id++)).c_str());
        CHECK(row && row->status == text && row->presence == Presence::Busy && row->blocked == "In a match");
    }
    profile = 600;
    for (const auto& [status, kind, text] : miis)
    {
        const auto* row = Row(online, (std::string("M") + std::to_string(profile++)).c_str());
        CHECK(row && row->status == text);
    }

    // What the game sends, from its snapshot.
    const auto kind_of = [](int mode, int online, int ranked, int lesson, int sides) {
        ChargedPresenceSnapshot s{};
        s.in_match = 1;
        s.mode = mode;
        s.online = online;
        s.ranked = ranked;
        s.strikers101 = lesson;
        s.human_sides = sides;
        const char* kind = platform::MatchKindOf(s);
        return std::string(kind ? kind : "-");
    };
    CHECK(kind_of(2, 0, 0, 0, 0) == "-"); // the title screen's demo
    CHECK(kind_of(0, 1, 1, 0, 1) == "ranked");
    CHECK(kind_of(0, 1, 0, 0, 2) == "friends");
    CHECK(kind_of(1, 1, 0, 0, 1) == "tournament");
    CHECK(kind_of(0, 0, 0, 1, 1) == "training");
    CHECK(kind_of(3, 0, 0, 0, 1) == "cup");
    CHECK(kind_of(4, 0, 0, 0, 1) == "challenge");
    CHECK(kind_of(0, 0, 0, 0, 1) == "cpu");
    CHECK(kind_of(0, 0, 0, 0, 0) == "cpu"); // CPU vs CPU
    CHECK(kind_of(0, 0, 0, 0, 3) == "local");
    ChargedPresenceSnapshot menus{};
    menus.mode = 0;
    CHECK(platform::MatchKindOf(menus) == nullptr);
}

int main()
{
    TestPlayer();
    TestMatchKinds();
    TestOnlineNow();
    TestAvailable();
    TestChallengeBlockers();
    TestVersions();
    TestSet();
    TestChallengeRules();
    TestSearch();
    if (failures)
    {
        std::fprintf(stderr, "%d online panel checks failed\n", failures);
        return 1;
    }
    std::printf("Online panel checks passed\n");
    return 0;
}
