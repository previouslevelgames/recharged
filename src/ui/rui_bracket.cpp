#include "ui/rui_bracket.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <utility>

namespace mscharged::ui
{
BracketLayout LayOutBracket(const std::vector<BracketSet>& sets)
{
    BracketLayout out;
    float top = 0.0f;
    bool any = false;
    for (int losers = 0; losers < 2; ++losers)
    {
        auto inSection = [losers](const BracketSet& set) { return (set.round < 0) == (losers == 1); };
        std::vector<int> rounds;
        std::map<std::string, std::size_t> byId; // the section's sets
        for (std::size_t n = 0; n < sets.size(); ++n)
            if (inSection(sets[n]))
            {
                rounds.push_back(std::abs(sets[n].round));
                byId.emplace(sets[n].id, n);
            }
        std::sort(rounds.begin(), rounds.end());
        rounds.erase(std::unique(rounds.begin(), rounds.end()), rounds.end());
        if (rounds.empty()) continue;
        if (any) top += kBracketSectionGap;
        any = true;
        if (losers) out.losers_top = top;
        const float cardsTop = top + kBracketHead;
        auto columnOf = [&](const BracketSet& set) {
            return int(std::lower_bound(rounds.begin(), rounds.end(), std::abs(set.round)) - rounds.begin());
        };
        // The sets each set's winners come from, in this section.
        auto feeders = [&](std::size_t n) {
            std::vector<std::size_t> from;
            for (const auto& prereq : sets[n].prereq)
                if (const auto it = byId.find(prereq); !prereq.empty() && it != byId.end() && it->second != n
                    && std::find(from.begin(), from.end(), it->second) == from.end())
                    from.push_back(it->second);
            return from;
        };
        // A tree from each last set back to the sets that feed it: a set with
        // none takes the next row, any other stands level with the middle of
        // its feeders, so each set's feeders sit together and no lines cross.
        std::vector<bool> fed(sets.size(), false);
        for (const auto& [id, n] : byId)
            for (const std::size_t from : feeders(n)) fed[from] = true;
        std::vector<std::size_t> roots;
        for (std::size_t n = 0; n < sets.size(); ++n)
            if (inSection(sets[n]) && !fed[n]) roots.push_back(n);
        // The last rounds first (the final, then a match for third place),
        // otherwise in the event's order.
        std::stable_sort(roots.begin(), roots.end(),
                         [&](std::size_t a, std::size_t b) { return columnOf(sets[a]) > columnOf(sets[b]); });
        std::vector<float> ys(sets.size(), 0.0f);
        std::vector<bool> placed(sets.size(), false);
        std::map<int, float> columnFloor; // a column's next free height
        float cursor = cardsTop;
        auto place = [&](std::size_t n, float y) {
            const int c = columnOf(sets[n]);
            const auto it = columnFloor.find(c);
            if (it != columnFloor.end()) y = std::max(y, it->second);
            ys[n] = y;
            placed[n] = true;
            columnFloor[c] = y + kBracketCardHeight + kBracketRowGap;
            cursor = std::max(cursor, y + kBracketCardHeight + kBracketRowGap);
        };
        std::vector<bool> visiting(sets.size(), false);
        std::function<void(std::size_t)> layOut = [&](std::size_t n) {
            if (placed[n] || visiting[n]) return;
            visiting[n] = true;
            float sum = 0.0f;
            int count = 0;
            for (const std::size_t from : feeders(n))
            {
                layOut(from);
                if (!placed[from]) continue;
                sum += ys[from];
                ++count;
            }
            place(n, count ? sum / float(count) : cursor);
        };
        for (const std::size_t root : roots) layOut(root);
        // (A loop of sets, which a bracket has not: the rest in order.)
        for (const auto& [id, n] : byId) layOut(n);

        float bottom = cardsTop;
        std::vector<std::size_t> order;
        for (const auto& [id, n] : byId) order.push_back(n);
        std::sort(order.begin(), order.end());
        std::vector<bool> captioned(rounds.size(), false);
        for (const std::size_t n : order)
        {
            const int c = columnOf(sets[n]);
            const float x = float(c) * (kBracketCardWidth + kBracketColumnGap);
            out.cells.push_back({n, c, losers == 1, x, ys[n]});
            bottom = std::max(bottom, ys[n] + kBracketCardHeight);
            if (!captioned[std::size_t(c)])
            {
                captioned[std::size_t(c)] = true;
                out.columns.push_back(
                    {c, losers == 1, x + kBracketCardWidth * 0.5f, top + kBracketHead * 0.72f, sets[n].round_text});
            }
        }
        out.width = std::max(out.width, float(rounds.size()) * (kBracketCardWidth + kBracketColumnGap)
                                            - kBracketColumnGap);
        top = bottom;
    }
    out.height = top;
    return out;
}

const BracketCell* YourBracketCell(const BracketLayout& layout, const std::vector<BracketSet>& sets)
{
    for (const auto& cell : layout.cells)
        if (sets[cell.set].yours) return &cell;
    return nullptr;
}
} // namespace mscharged::ui
