#pragma once

#include <cstddef>
#include <string>
#include <vector>

// A tournament's bracket (one pool) for the UI kit's List::Bracket: its sets
// by round in columns, the winners' bracket above the losers', and where
// each set's entrants come from. The layout is plain arithmetic, apart from
// the drawing.
namespace mscharged::ui
{
struct BracketEntrant
{
    std::string name;  // empty: still to be decided
    std::string score; // its games in the set, once reported
    bool winner = false;
    bool you = false;  // the player
};

enum class BracketSetState
{
    Waiting, // not started
    Going,   // being played, or called
    Done,
};

// A game a set played: its line under the bracket, and what the game's
// results screen shows of it (the captains, eTeamID, and the goals, home
// first).
struct BracketGame
{
    std::string text;
    int captains[2] = {-1, -1};
    int goals[2] = {0, 0};
};

struct BracketSet
{
    std::string id;
    int round = 0;          // > 0 the winners' bracket (or a single one), < 0 the losers'
    std::string round_text; // its column's caption ("Winners Semi-Final")
    BracketEntrant slots[2];
    // The sets whose winners come here (empty for a seed, and for a set's
    // loser): the lines, and where the set stands.
    std::string prereq[2];
    BracketSetState state = BracketSetState::Waiting;
    bool yours = false; // the player's next set
    std::vector<BracketGame> games; // the games it played: its card is picked to list them
};

// Design pixels: a set's card, the room between columns (for the lines) and
// between cards, a section's head (its title, then its columns' captions),
// the room between the winners' and the losers' sections.
inline constexpr float kBracketCardWidth = 300.0f, kBracketCardHeight = 80.0f;
inline constexpr float kBracketColumnGap = 56.0f, kBracketRowGap = 18.0f;
inline constexpr float kBracketHead = 84.0f, kBracketSectionGap = 36.0f;

struct BracketCell
{
    std::size_t set; // its index in the sets
    int column;
    bool losers;
    float x, y; // its card's top left, from the bracket's
};
struct BracketColumn
{
    int column;
    float x, y; // its caption's middle
    std::string caption;
};
struct BracketLayout
{
    std::vector<BracketCell> cells;
    std::vector<BracketColumn> columns;
    float width = 0.0f, height = 0.0f;
    float losers_top = -1.0f; // the losers' section's top, -1 without one
};

// The rounds in columns (by their number: the winners' up, the losers' by
// their size), laid out as trees from each section's last sets back: a set
// level with the middle of the sets its winners come from (in its own
// section), a set fed by none on the next row, so no lines cross; the sets
// of a column apart.
BracketLayout LayOutBracket(const std::vector<BracketSet>& sets);
// The player's set's cell, or null.
const BracketCell* YourBracketCell(const BracketLayout& layout, const std::vector<BracketSet>& sets);
} // namespace mscharged::ui
