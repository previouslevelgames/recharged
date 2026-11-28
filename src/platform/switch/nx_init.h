#pragma once

// The CPU runs boosted from start until the graphics are prepared for the game.
extern "C" void charged_switch_end_startup_boost(void);
// And again while the game's task state (nlTaskManager) is a loading one.
extern "C" void charged_switch_follow_loading(unsigned task_state);
