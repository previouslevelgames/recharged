#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OriginalDSPBootData {
    const unsigned char* bytes;
    size_t count;
} OriginalDSPBootData;
OriginalDSPBootData OriginalDSPBootDataForFixture(void);
#ifdef __cplusplus
}
#endif
