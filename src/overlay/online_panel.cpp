#include "overlay/online_panel.h"

#include <algorithm>

namespace mscharged::overlay
{
int ChosenOnlinePanelPreset(const OnlinePanelModel& model)
{
    return model.presets.empty() ? 0 : std::clamp(model.chosen_preset, 0, int(model.presets.size()) - 1);
}

void StartOnlinePanelSetup(OnlinePanelState& state, const OnlinePanelModel& model, const std::string& key)
{
    state.setup_for = key;
    state.preset = ChosenOnlinePanelPreset(model);
    state.rules = model.presets.empty() ? platform::MatchRules{} : model.presets[std::size_t(state.preset)].rules;
}

bool OnlinePanelSearchRunning(const OnlinePanelModel& model, OnlinePanelState& state, double now)
{
    if (model.search.active || now - state.search_sent > 10.0) state.search_sent = -100.0;
    return model.search.active || state.search_sent > 0.0;
}

OnlinePanelChallenge MakeOnlinePanelChallenge(const OnlinePanelPreset& preset, const platform::MatchRules& rules,
                                              std::int64_t player)
{
    const bool changed = !(rules == preset.rules);
    OnlinePanelChallenge challenge;
    challenge.player = player;
    challenge.name = preset.name;
    // (The server keeps names of up to 48 characters.)
    if (changed && preset.name.size() + 10 <= 48) challenge.name += " (changed)";
    challenge.rules = rules;
    challenge.rule_set = changed ? 0 : preset.rule_set;
    return challenge;
}
} // namespace mscharged::overlay
