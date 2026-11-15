// The bracket's layout (ui/rui_bracket.h): rounds in columns, a set level
// with the sets its entrants come from, the sets of a column apart, the
// losers' section under the winners'.
#include "ui/rui_bracket.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mscharged::ui;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

BracketSet Set(const char* id, int round, const char* a = "", const char* b = "")
{
    BracketSet set;
    set.id = id;
    set.round = round;
    set.round_text = "Round " + std::to_string(round);
    set.prereq[0] = a;
    set.prereq[1] = b;
    return set;
}

const BracketCell* Cell(const BracketLayout& layout, const std::vector<BracketSet>& sets, const char* id)
{
    for (const auto& cell : layout.cells)
        if (sets[cell.set].id == id) return &cell;
    return nullptr;
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

void SingleElimination()
{
    // Four players: two semi-finals, then the final between their winners.
    const std::vector<BracketSet> sets{Set("f", 2, "s1", "s2"), Set("s1", 1), Set("s2", 1)};
    const BracketLayout layout = LayOutBracket(sets);
    CHECK(layout.cells.size() == 3);
    CHECK(layout.columns.size() == 2);
    CHECK(layout.losers_top < 0.0f);
    CHECK(Near(layout.width, 2 * kBracketCardWidth + kBracketColumnGap));
    const BracketCell *s1 = Cell(layout, sets, "s1"), *s2 = Cell(layout, sets, "s2"), *f = Cell(layout, sets, "f");
    CHECK(s1 && s2 && f);
    if (!s1 || !s2 || !f) return;
    CHECK(s1->column == 0 && s2->column == 0 && f->column == 1);
    CHECK(Near(s1->y, kBracketHead));
    CHECK(Near(s2->y, s1->y + kBracketCardHeight + kBracketRowGap));
    // The final between the semi-finals.
    CHECK(Near(f->y, (s1->y + s2->y) * 0.5f));
    CHECK(Near(f->x, kBracketCardWidth + kBracketColumnGap));
    CHECK(Near(layout.height, s2->y + kBracketCardHeight));
    bool captioned = false;
    for (const auto& column : layout.columns)
        if (column.column == 0) captioned = column.caption == "Round 1";
    CHECK(captioned);
}

// Whether the line from a to b and the line from c to d cross (their ends'
// heights in the wrong order).
bool Crossing(const BracketCell& a, const BracketCell& b, const BracketCell& c, const BracketCell& d)
{
    return (a.y < c.y) != (b.y < d.y) && a.y != c.y && b.y != d.y;
}

void NoCrossings()
{
    // start.gg's order is not the bracket's: the final takes s2 first, the
    // semi-finals take the quarter-finals out of their order.
    const std::vector<BracketSet> sets{Set("q1", 1), Set("q2", 1), Set("q3", 1), Set("q4", 1),
                                       Set("sA", 2, "q1", "q3"), Set("sB", 2, "q4", "q2"), Set("f", 3, "sB", "sA")};
    const BracketLayout layout = LayOutBracket(sets);
    struct Line
    {
        const char *from, *to;
    };
    const Line lines[] = {{"q1", "sA"}, {"q3", "sA"}, {"q4", "sB"}, {"q2", "sB"}, {"sB", "f"}, {"sA", "f"}};
    for (const Line& one : lines)
        for (const Line& other : lines)
        {
            const BracketCell *a = Cell(layout, sets, one.from), *b = Cell(layout, sets, one.to);
            const BracketCell *c = Cell(layout, sets, other.from), *d = Cell(layout, sets, other.to);
            CHECK(a && b && c && d);
            if (a && b && c && d && a->column == c->column) CHECK(!Crossing(*a, *b, *c, *d));
        }
    // Each semi-final between its two quarter-finals, the final between them.
    const BracketCell *sA = Cell(layout, sets, "sA"), *sB = Cell(layout, sets, "sB"), *f = Cell(layout, sets, "f");
    const BracketCell *q4 = Cell(layout, sets, "q4"), *q2 = Cell(layout, sets, "q2");
    if (sA && sB && f && q4 && q2)
    {
        CHECK(sB->y < sA->y); // the final's first
        CHECK(Near(sB->y, (q4->y + q2->y) * 0.5f));
        CHECK(Near(f->y, (sA->y + sB->y) * 0.5f));
    }
}

void ThirdPlace()
{
    // The match for third place takes the semi-finals' losers: no set of
    // theirs feeds it (the page leaves losers' prereqs out), so it goes on
    // a row of its own, under the bracket, not level with the final.
    const std::vector<BracketSet> sets{Set("s1", 1), Set("s2", 1), Set("f", 2, "s1", "s2"), Set("third", 2)};
    const BracketLayout layout = LayOutBracket(sets);
    const BracketCell *s2 = Cell(layout, sets, "s2"), *f = Cell(layout, sets, "f"), *third = Cell(layout, sets, "third");
    CHECK(s2 && f && third);
    if (s2 && f && third)
    {
        CHECK(third->column == f->column);
        CHECK(third->y >= s2->y + kBracketCardHeight + kBracketRowGap - 0.01f);
    }
}

void DoubleElimination()
{
    // Losers' round 1 takes the semi-finals' losers: in its own section,
    // under the winners'.
    std::vector<BracketSet> sets{Set("s1", 1), Set("s2", 1), Set("wf", 2, "s1", "s2"), Set("l1", -1, "s1", "s2"),
                                 Set("lf", -2, "l1", "wf")};
    sets[4].yours = true;
    const BracketLayout layout = LayOutBracket(sets);
    CHECK(layout.cells.size() == 5);
    CHECK(layout.columns.size() == 4);
    const BracketCell *s2 = Cell(layout, sets, "s2"), *l1 = Cell(layout, sets, "l1"), *lf = Cell(layout, sets, "lf");
    CHECK(s2 && l1 && lf);
    if (!s2 || !l1 || !lf) return;
    CHECK(Near(layout.losers_top, s2->y + kBracketCardHeight + kBracketSectionGap));
    CHECK(l1->losers && lf->losers && l1->column == 0 && lf->column == 1);
    CHECK(Near(l1->y, layout.losers_top + kBracketHead));
    // Level with l1, its only entrant's set in its section.
    CHECK(Near(lf->y, l1->y));
    CHECK(YourBracketCell(layout, sets) == lf);
}

void Apart()
{
    // Two sets that would sit at the same height: the second goes under.
    const std::vector<BracketSet> sets{Set("a", 1), Set("x", 2, "a"), Set("y", 2, "a")};
    const BracketLayout layout = LayOutBracket(sets);
    const BracketCell *x = Cell(layout, sets, "x"), *y = Cell(layout, sets, "y");
    CHECK(x && y);
    if (x && y) CHECK(Near(y->y, x->y + kBracketCardHeight + kBracketRowGap));
    CHECK(YourBracketCell(layout, sets) == nullptr);
    CHECK(LayOutBracket({}).cells.empty() && LayOutBracket({}).height == 0.0f);
}
} // namespace

int main()
{
    SingleElimination();
    DoubleElimination();
    Apart();
    NoCrossings();
    ThirdPlace();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("rui_bracket: all passed");
    return failures ? 1 : 0;
}
