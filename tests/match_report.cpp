// The launcher's match stats: the game's pictures read from a front-end
// bundle (names, hashes, the three texture formats, damaged bundles) and the
// report a replay's stats make (totals side by side, goals in order with the
// score, which shots scored, items, possession, the players' table).
#include "launcher/game_pictures.h"
#include "launcher/match_report.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mscharged;
using namespace mscharged::launcher;

namespace
{
int sChecks = 0;
void Require(bool condition, const std::string& message)
{
    ++sChecks;
    if (!condition) throw std::runtime_error(message);
}

void PutBE16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value)
{
    out[at] = std::uint8_t(value >> 8);
    out[at + 1] = std::uint8_t(value);
}
void PutBE32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value)
{
    for (int byte = 0; byte < 4; ++byte) out[at + std::size_t(byte)] = std::uint8_t(value >> (24 - 8 * byte));
}

std::vector<std::uint8_t> Texture(std::uint32_t format, int width, int height, const std::vector<std::uint8_t>& level)
{
    std::vector<std::uint8_t> texture(32, 0);
    PutBE32(texture, 0, 1);
    PutBE32(texture, 4, format);
    PutBE16(texture, 14, std::uint16_t(width));
    PutBE16(texture, 16, std::uint16_t(height));
    texture.insert(texture.end(), level.begin(), level.end());
    return texture;
}

// A bundle as the game's BundleFile has it: 32-byte sectors, the directory
// in the second, the files after it.
std::vector<std::uint8_t> Bundle(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files)
{
    std::vector<std::uint8_t> bundle(64 + 32 * ((files.size() * 12 + 31) / 32), 0);
    PutBE32(bundle, 0, 32);
    PutBE32(bundle, 4, std::uint32_t(files.size()));
    PutBE32(bundle, 8, 1);
    for (std::size_t n = 0; n < files.size(); ++n)
    {
        while (bundle.size() % 32) bundle.push_back(0);
        PutBE32(bundle, 32 + 12 * n, GameNameHash(files[n].first));
        PutBE32(bundle, 32 + 12 * n + 4, std::uint32_t(bundle.size() / 32));
        PutBE32(bundle, 32 + 12 * n + 8, std::uint32_t(files[n].second.size()));
        bundle.insert(bundle.end(), files[n].second.begin(), files[n].second.end());
    }
    return bundle;
}

// RGB5A3, 4x4: opaque red (0xFC00) but texel (1, 0), transparent black.
std::vector<std::uint8_t> RedRGB5A3()
{
    std::vector<std::uint8_t> level;
    for (int n = 0; n < 16; ++n)
    {
        level.push_back(n == 1 ? 0x00 : 0xFC);
        level.push_back(0x00);
    }
    return level;
}

bool Texel(const Picture& picture, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    const std::size_t at = (std::size_t(y) * std::size_t(picture.width) + std::size_t(x)) * 4;
    return picture.rgba[at] == r && picture.rgba[at + 1] == g && picture.rgba[at + 2] == b && picture.rgba[at + 3] == a;
}

void Pictures()
{
    // The names as the disc has them (fe/captain_icons/captain_icons_mario
    // is entry 458562f5 of a US disc's captainiconsui.res).
    Require(GameNameHash("fe/captain_icons/captain_icons_mario") == 0x458562f5u, "the game's name hash");
    Require(CaptainIconPath(0) == "fe/captain_icons/captain_icons_mario"
                && CaptainIconPath(11) == "fe/captain_icons/captain_icons_petey" && CaptainIconPath(12).empty()
                && CaptainIconPath(-1).empty(),
            "captain icon names");
    Require(SidekickPicturePath(2, 7) == "fe/sidekick_images/attributes_hammerbro_wario"
                && GameNameHash(SidekickPicturePath(2, 7)) == 0x6a2eb788u
                && SidekickPicturePath(7, 1, true) == "fe/sidekick_images/attributes_shyguy_bowser_alt"
                && SidekickPicturePath(8, 1).empty(),
            "sidekick picture names");

    // CMPR, 8x8: four blocks; the first in four-colour mode (white, black and
    // the thirds between), the second in three-colour mode with a
    // transparent texel.
    std::vector<std::uint8_t> cmpr(32, 0);
    PutBE16(cmpr, 0, 0xFFFF); // white
    PutBE16(cmpr, 2, 0x0000); // black
    cmpr[4] = 0x1B;           // row 0: 0 1 2 3
    PutBE16(cmpr, 8, 0x0000);
    PutBE16(cmpr, 10, 0xF800); // three-colour mode: red
    cmpr[12] = 0xFF;           // row 0: transparent
    PutBE16(cmpr, 16, 0x07E0); // green, then blue: block 3 all colour 0
    PutBE16(cmpr, 24, 0x001F);
    PutBE16(cmpr, 26, 0x001F);
    const auto icons = Bundle({{"fe/captain_icons/captain_icons_mario", Texture(2, 8, 8, cmpr)},
                               {"other", Texture(1, 4, 4, RedRGB5A3())},
                               {"rgba", Texture(3, 4, 4, std::vector<std::uint8_t>(64, 0))}});
    const ByteView bundle{icons.data(), icons.size()};
    Picture picture;
    Require(DecodePicture(BundleEntry(bundle, CaptainIconPath(0)), picture) && picture.width == 8
                && picture.height == 8 && picture.rgba.size() == 8 * 8 * 4,
            "a CMPR picture decodes");
    Require(Texel(picture, 0, 0, 255, 255, 255, 255) && Texel(picture, 1, 0, 0, 0, 0, 255)
                && Texel(picture, 2, 0, 170, 170, 170, 255) && Texel(picture, 3, 0, 85, 85, 85, 255),
            "four-colour mode");
    Require(Texel(picture, 4, 0, 0, 0, 0, 0) && Texel(picture, 4, 1, 0, 0, 0, 255), "three-colour mode");
    Require(Texel(picture, 0, 4, 0, 255, 0, 255) && Texel(picture, 7, 7, 0, 0, 255, 255), "the blocks' order");
    Require(DecodePicture(BundleEntry(bundle, "other"), picture) && picture.width == 4
                && Texel(picture, 0, 0, 255, 0, 0, 255) && Texel(picture, 3, 3, 255, 0, 0, 255)
                && Texel(picture, 1, 0, 0, 0, 0, 0),
            "an RGB5A3 picture (opaque red, a transparent texel)");
    Require(DecodePicture(BundleEntry(bundle, "rgba"), picture) && picture.height == 4, "an RGBA8 picture");
    Require(BundleEntry(bundle, "missing").empty(), "a missing name");
    Require(BundleEntry({icons.data(), 20}, CaptainIconPath(0)).empty(), "a cut bundle");
    auto damaged = icons;
    PutBE32(damaged, 32 + 8, 0x10000000); // the first file's length
    Require(BundleEntry({damaged.data(), damaged.size()}, CaptainIconPath(0)).empty(), "a file past the bundle's end");
    PutBE32(damaged, 4, 0x7FFFFFFF);
    Require(BundleEntry({damaged.data(), damaged.size()}, "other").empty(), "too many files");
    const auto cut = Texture(2, 8, 8, std::vector<std::uint8_t>(16, 0));
    Require(!DecodePicture({cut.data(), cut.size()}, picture), "a cut texture is refused");
    const auto unknown = Texture(9, 8, 8, std::vector<std::uint8_t>(64, 0));
    Require(!DecodePicture({unknown.data(), unknown.size()}, picture), "another format is refused");
    const auto huge = Texture(2, 4096, 8, std::vector<std::uint8_t>(64, 0));
    Require(!DecodePicture({huge.data(), huge.size()}, picture), "an oversized texture is refused");
}

replay::StatEvent Event(std::uint32_t frame, int stat, int side, int player, float x = 0.0f, float y = 0.0f)
{
    replay::StatEvent event;
    event.frame = frame;
    event.stat = std::uint8_t(stat);
    event.side = std::uint8_t(side);
    event.player = std::int8_t(player);
    event.x = x;
    event.y = y;
    return event;
}

void Report()
{
    replay::Setup setup;
    setup.home_team = 7;
    setup.away_team = 1;
    setup.home_sidekicks[0] = 2;
    setup.away_sidekicks[2] = 4;
    for (auto& side : setup.playing_sides) side = -1;
    setup.playing_sides[0] = 0;
    setup.playing_sides[3] = 1;
    setup.steps = 50 * 180;
    setup.score[0] = 4;
    setup.score[1] = 2;

    const MatchReport none = BuildMatchReport(setup);
    Require(!none.has_stats && none.goals.empty() && none.sides[0].captain == 7
                && none.sides[0].sidekicks[0] == 2 && none.sides[1].sidekicks[2] == 4
                && none.sides[0].pads == std::vector<int>{0} && none.sides[1].pads == std::vector<int>{3}
                && none.length == 180.0f && none.score[0] == 4,
            "a replay without stats: the sides and score only");

    replay::MatchStats& stats = setup.stats;
    stats.kept = 1;
    std::strcpy(stats.names[1], "Rival");
    stats.name_kinds[1] = replay::kNameProfile;
    stats.field[0] = 30.0f;
    stats.field[1] = 18.0f;
    stats.first_frame = 1000;
    stats.counts[0][0][0x00] = 3; // low shots
    stats.counts[0][1][0x02] = 1; // a high shot
    stats.counts[0][0][0x04] = 1; // a Skillshot
    stats.counts[1][2][0x00] = 2;
    stats.counts[0][0][0x03] = 4; // on goal
    stats.counts[0][3][0x12] = 5; // hits
    stats.counts[1][1][0x14] = 2; // steals
    stats.counts[0][0][0x16] = 300; // possession
    stats.counts[1][4][0x16] = 100;
    stats.counts[0][2][0x19] = 3; // items
    stats.counts[0][0][0x0D] = 12; // passes
    auto& e = stats.events;
    int n = 0;
    e[n++] = Event(1100, 0x00, 0, 0, 20.0f, 3.0f);  // shot, 2 s
    e[n] = Event(1150, 0x0B, 0, 0);                  // goal by the captain, assisted by player 2
    e[n].other = 2;
    e[n++].kind = 1;
    e[n++] = Event(1200, 0x00, 1, 2, -20.0f, 0.0f); // the away side's shot, no goal
    e[n++] = Event(1300, 0x19, 0, 2, 1.0f, 1.0f);   // a triple banana
    e[n - 1].other = 4;
    e[n - 1].count = 3;
    e[n++] = Event(1400, 0x19, 0, 2);               // and another
    e[n - 1].other = 4;
    e[n - 1].count = 3;
    e[n++] = Event(1500, 0x19, 0, 2);               // a mushroom
    e[n - 1].other = 7;
    e[n] = Event(1600, 0x0B, 1, 3);                 // an own goal for away
    e[n].flags = replay::kOwnGoal;
    e[n++].kind = 1;
    e[n] = Event(2000, 0x0B, 0, 0);                 // a Mega Strike: two goals...
    e[n].kind = 6;
    e[n++].count = 2;
    e[n++] = Event(2000, 0x09, 0, 0, 25.0f, -2.0f); // ...and the Mega Strike after them
    e[n] = Event(2100, 0x12, 0, 3);                 // a hit with the match clock
    e[n++].clock = 61.5f;
    e[n++] = Event(2200, 0x00, 0, 1, 10.0f, 0.0f);  // a shot long before the next goal
    e[n] = Event(2600, 0x0B, 0, 1);
    e[n++].kind = 1;
    e[n] = Event(2610, 0x0B, 1, 0);                 // away scores without a shot
    e[n++].kind = 1;
    stats.event_count = std::uint16_t(n);
    setup.resumes = 1;
    setup.start_score[0] = 0;
    setup.start_score[1] = 0;

    const MatchReport report = BuildMatchReport(setup);
    Require(report.has_stats && report.sides[1].name == "Rival" && report.sides[1].name_kind == replay::kNameProfile
                && report.sides[0].name.empty() && report.field_x == 30.0f,
            "the names and the field");
    Require(report.goals.size() == 5, "five goals");
    Require(report.goals[0].seconds == 3.0f && report.goals[0].player == 0 && report.goals[0].assist == 2
                && report.goals[0].score == std::array<int, 2>{1, 0},
            "the first goal: when, who, the score after");
    Require(report.goals[1].own_goal && report.goals[1].player == -1 && report.goals[1].side == 1
                && report.goals[1].score == std::array<int, 2>{1, 1},
            "an own goal counts for the other side, without a scorer");
    Require(report.goals[2].count == 2 && report.goals[2].kind == 6 && report.goals[2].score[0] == 3,
            "a Mega Strike's goals");
    Require(report.goals[4].score == std::array<int, 2>{4, 2}, "the score at the end");
    Require(report.shots.size() == 4, "four shots");
    Require(report.shots[0].scored && report.shots[0].x == 20.0f, "the first shot scored");
    Require(!report.shots[1].scored, "the away shot did not");
    Require(report.shots[2].kind == ShotKind::MegaStrike && report.shots[2].scored, "the Mega Strike scored");
    Require(!report.shots[3].scored, "a goal 8 s after a shot is not that shot's");
    Require(report.items.size() == 3 && report.item_counts[0].size() == 2 && report.item_counts[0][0].type == 4
                && report.item_counts[0][0].count == 3 && report.item_counts[0][0].uses == 2
                && report.item_counts[0][1].type == 7 && report.item_counts[1].empty(),
            "items by type and count, most used first");
    const auto row = [&](StatRow which, int side) { return report.rows[std::size_t(which)][std::size_t(side)]; };
    Require(row(StatRow::Goals, 0) == 4 && row(StatRow::Goals, 1) == 2, "the goals row is the score");
    Require(row(StatRow::Shots, 0) == 3 + 1 + 1 + 1 && row(StatRow::Shots, 1) == 2,
            "shots: charged, Skillshots and Mega Strikes");
    Require(row(StatRow::MegaStrikes, 0) == 1 && row(StatRow::ShotsOnGoal, 0) == 4 && row(StatRow::Hits, 0) == 5
                && row(StatRow::Steals, 1) == 2 && row(StatRow::Items, 0) == 3 && row(StatRow::Passes, 0) == 12,
            "the other rows");
    Require(row(StatRow::Possession, 0) == 75 && row(StatRow::Possession, 1) == 25, "possession in percent");
    const auto player = [&](int side, int who, PlayerColumn column) {
        return report.players[std::size_t(side)][std::size_t(who)][std::size_t(column)];
    };
    Require(player(0, 0, PlayerColumn::Goals) == 3 && player(0, 2, PlayerColumn::Assists) == 1
                && player(1, 3, PlayerColumn::Goals) == 0 && player(0, 1, PlayerColumn::Goals) == 1,
            "goals and assists by player, own goals left out");
    Require(player(0, 0, PlayerColumn::Shots) == 3 + 1 + 1 && player(0, 3, PlayerColumn::Hits) == 5,
            "the players' other columns");
    Require(report.moments.size() == 9 && report.moments.back().kind == Moment::Goal, "the timeline's moments");
    Require(report.moments[6].kind == Moment::Hit && report.moments[6].seconds == 61.5f, "the match clock wins");
    Require(report.length == 180.0f, "the match's length");
    Require(ClockText(0.0f) == "0:00" && ClockText(61.9f) == "1:01" && ClockText(600.0f) == "10:00"
                && ClockText(-3.0f) == "0:00",
            "clock texts");
}
} // namespace

int main()
{
    try
    {
        Pictures();
        Report();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "match report test failed: %s\n", error.what());
        return 1;
    }
    std::printf("match report: %d checks passed\n", sChecks);
    return 0;
}
