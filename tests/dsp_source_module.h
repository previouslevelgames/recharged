#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OriginalDSPData {
    void* code;
    uint32_t code_bytes;
    void* dram;
    uint32_t dram_bytes;
    uint16_t initial_vector, resume_vector;
} OriginalDSPData;
OriginalDSPData OriginalDSPDataForFixture(void);
#ifdef __cplusplus
}
#endif
