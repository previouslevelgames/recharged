// Tracy's client does not build for Horizon (cmake/AuroraHost.cmake), and libnx
// threads carry no names: Aurora's names for its threads have nowhere to go.
#include <common/TracySystem.hpp>

namespace tracy
{
void SetThreadName(const char*) {}
}
