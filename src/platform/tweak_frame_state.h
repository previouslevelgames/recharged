#pragma once

// The frame rate option's state ([display] frame_rate, game_tweaks.cpp) at
// the start of a frame. BeginFrameTask computes it from the frame's real
// length, and the cameras, blinks, texture animations and presentation read
// it: a rollback that replays a frame puts back the state that frame started
// with, then the replayed BeginFrameTask computes it again.
struct MschargedTweakFrameState
{
    float frames, phase;
    unsigned long count;
    bool frame;
};

extern "C" void mscharged_tweak_save_frame_state(MschargedTweakFrameState* state);
extern "C" void mscharged_tweak_load_frame_state(const MschargedTweakFrameState* state);
