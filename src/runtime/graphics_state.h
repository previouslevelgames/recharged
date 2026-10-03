#pragma once

namespace mscharged
{
// Select the original state/matrix stages after graphics memory is available.
// Views, targets and material programs have separate session owners; full
// original glStartup remains pending.
void InitializeOriginalGraphicsState();
}
