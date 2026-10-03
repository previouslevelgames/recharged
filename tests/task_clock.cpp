// Compile the exact prepared ticker arithmetic with deterministic test inputs.
// Rename only its OS providers so Aurora retains its ordinary clock symbols.
#define OSGetTick ChargedFixtureGetTick
#define ChargedGetBusClock ChargedFixtureGetBusClock
#include "NL/nlTicker.cpp"
