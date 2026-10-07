#pragma once

#include <cstdint>

struct SaveGateObservation {
    unsigned checks;
    unsigned source_user_callbacks;
    unsigned original_task_runs;
    unsigned allocated_read_bytes;
    unsigned raw_read_result;
};

extern "C" void SaveGateCheck(bool, const char*);
extern "C" void SaveGateServiceIOS(bool expect_work);
extern "C" bool SaveGateIOSPending();
extern "C" bool SaveGateOwnerContextRestored();
extern "C" void SaveGateCold(SaveGateObservation*);
extern "C" void SaveGateRun(SaveGateObservation*);
