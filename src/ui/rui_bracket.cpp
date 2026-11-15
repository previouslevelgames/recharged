#include "ui/rui_bracket.h"

#include <algorithm>
#include <cstdlib>
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
        for (const auto& set : sets)
            if (inSection(set)) rounds.push_back(std::abs(set.round));
        std::sort(rounds.begin(), rounds.end());
        rounds.erase(std::unique(rounds.begin(), rounds.end()), rounds.end());
        if (rounds.empty()) continue;
        if (any) top += kBracketSectionGap;
        any = true;
        if (losers) out.losers_top = top;
        const float cardsTop = top + kBracketHead;
        float bottom = cardsTop;
        // The middle of each set placed in this section, by its id.
        std::map<std::string, float> middles;
        for (std::size_t c = 0; c < rounds.size(); ++c)
        {
            const float x = float(c) * (kBracketCardWidth + kBracketColumnGap);
            // Where each set of the column would go: level with its
            // entrants' sets, else after the one before.
            std::vector<std::pair<float, std::size_t>> wanted;
            float next = cardsTop;
            for (std::size_t n = 0; n < sets.size(); ++n)
            {
                const BracketSet& set = sets[n];
                if (!inSection(set) || std::abs(set.round) != rounds[c]) continue;
                float sum = 0.0f;
                int count = 0;
                for (const auto& prereq : set.prereq)
                    if (const auto it = middles.find(prereq); !prereq.empty() && it != middles.end())
                    {
                        sum += it->second;
                        ++count;
                    }
                const float y = count ? sum / float(count) - kBracketCardHeight * 0.5f : next;
                wanted.emplace_back(y, n);
                next = std::max(next, y) + kBracketCardHeight + kBracketRowGap;
            }
            std::stable_sort(wanted.begin(), wanted.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            float floor = cardsTop;
            for (const auto& [want, n] : wanted)
            {
                const float y = std::max(want, floor);
                floor = y + kBracketCardHeight + kBracketRowGap;
                out.cells.push_back({n, int(c), losers == 1, x, y});
                middles[sets[n].id] = y + kBracketCardHeight * 0.5f;
                bottom = std::max(bottom, y + kBracketCardHeight);
            }
            out.columns.push_back({int(c), losers == 1, x + kBracketCardWidth * 0.5f,
                                   top + kBracketHead * 0.72f, sets[wanted.front().second].round_text});
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
