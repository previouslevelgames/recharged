#pragma once

#include <cstdint>

struct FlashGateObservation {
    unsigned checks;
    unsigned source_user_callbacks;
    unsigned original_task_runs;
    unsigned allocated_read_bytes;
    unsigned raw_read_result;
};

extern "C" void FlashGateCheck(bool, const char*);
extern "C" void FlashGateServiceIOS(bool expect_work);
extern "C" bool FlashGateIOSPending();
extern "C" bool FlashGateOwnerContextRestored();
extern "C" void FlashGateCold(FlashGateObservation*);
extern "C" void FlashGateRun(FlashGateObservation*);
