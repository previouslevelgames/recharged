#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The Wii's qsort (MSL heapsort, cmake/WiiQsort.cmake): the same order on
// every host, equal elements included. Host qsorts order equal elements
// differently, and online every machine must sort as the others do.
void ChargedWii_qsort(void* base, size_t count, size_t size,
                      int (*compare)(const void*, const void*));

#ifdef __cplusplus
}
#endif
