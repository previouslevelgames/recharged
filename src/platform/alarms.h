#pragma once
#include <cstddef>
namespace mscharged::platform {
// Borrow one real SDK clock/context on its actual hardware owner. Retire all
// alarm queue references before source storage and SDK arenas are released.
void InitializeNativeAlarms();
void ShutdownNativeAlarms();
// One actual decrementer delivery per safe point. Masked, foreign and reentrant
// calls retain due requests. This does not service/present graphics or audio.
std::size_t ServiceNativeAlarms();
}
