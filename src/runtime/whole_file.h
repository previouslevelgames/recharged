#pragma once

namespace mscharged
{
// Query on the NL servicing thread. Tokens are never reused. A disappeared
// request without its completion means read failure or file-service shutdown.
bool WholeFileLoadPending(unsigned handle);
}
