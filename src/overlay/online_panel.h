#pragma once

#include "overlay/online_panel_model.h"

#include <cstdint>
#include <optional>
#include <string>

// What the menus' online pages (src/ui/rui_pages.cpp) hand the host: what the
// player did, and the challenge setup they keep between frames.
namespace mscharged::overlay
{
// A challenge as its setup sends it: by one of the server's rule sets as it
// is (`rule_set`), else by `rules`, under the preset's name.
struct OnlinePanelChallenge
{
    std::int64_t player = 0;
    std::string name;
    platform::MatchRules rules;
    std::int64_t rule_set = 0;
};

// What the player did on the online pages this frame; the host acts on it
// (asks the profile service). Fresh for every frame.
struct OnlinePanelActions
{
    std::optional<OnlinePanelChallenge> challenge;
    // A search for a random opponent by these rules (`player` 0), or its stop.
    std::optional<OnlinePanelChallenge> search;
    bool stop_search = false;
    std::optional<std::int64_t> add_friend;    // a player's id
    std::optional<std::int64_t> remove_friend; // a player's id
    std::optional<std::int32_t> add_mii_friend;    // a Mii profile
    std::optional<std::int32_t> remove_mii_friend; // a Mii profile
    std::optional<bool> set_available; // open to challenges, or not
    // The preset a challenge's setup opens with from now on (its name, one
    // of OnlinePanelModel::presets): the one a challenge was sent with.
    std::optional<std::string> set_challenge_rules;
    // The set's (OnlinePanelSet::match): accept or decline its challenge,
    // Ready to play!, withdraw or cancel it, play it again.
    bool accept = false, decline = false, ready = false, cancel = false, rematch = false;

    bool Any() const
    {
        return challenge || search || stop_search || add_friend || remove_friend || add_mii_friend
            || remove_mii_friend || set_available || set_challenge_rules || accept || decline || ready || cancel
            || rematch;
    }
};

// What the pages keep between frames: the challenge being set up, its player's
// key, the preset it started from (in OnlinePanelModel::presets) and its
// rules as the player changed them.
struct OnlinePanelState
{
    std::string setup_for;
    int preset = 0;
    platform::MatchRules rules;
    // When the search's setup asked for the search (ImGui::GetTime()), until
    // the pages show it running; negative: not waiting for one.
    double search_sent = -100.0;
};

// The key of the setup of a search for a random opponent (OnlinePanelState::
// setup_for; a challenge's is its player's row key).
inline constexpr const char* kOnlinePanelSearchKey = "search";
// The preset a setup starts from: the one chosen last (0 without presets).
int ChosenOnlinePanelPreset(const OnlinePanelModel& model);
// A setup for `key` starts from that preset, with its rules.
void StartOnlinePanelSetup(OnlinePanelState& state, const OnlinePanelModel& model, const std::string& key);
// Whether the search runs, or was asked for at most 10 s ago (`now`:
// ImGui::GetTime()) and the server has not said yet; forgets the request
// once it shows or is that old.
bool OnlinePanelSearchRunning(const OnlinePanelModel& model, OnlinePanelState& state, double now);
// The challenge (or search, player 0) `rules` make from `preset`: by the
// server's rule set itself while nothing changed (servers before 1.2.3 take
// only theirs), else the preset's name marked changed.
OnlinePanelChallenge MakeOnlinePanelChallenge(const OnlinePanelPreset& preset, const platform::MatchRules& rules,
                                              std::int64_t player);

} // namespace mscharged::overlay
