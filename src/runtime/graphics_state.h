#pragma once

namespace mscharged
{
// Select the original state/matrix stages after graphics memory is available.
// Views, targets, material programs and the full glStartup remain separate work.
void InitializeOriginalGraphicsState();
}
