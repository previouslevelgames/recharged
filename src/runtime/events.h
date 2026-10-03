#pragma once
#include <string>

namespace mscharged
{
// Runs original registry/listener operations after game memory is initialized.
// All event objects and owners must be destroyed before registry shutdown.
std::string VerifyStartupEvents();
std::string VerifyStartupQueuedEvents();
// Final teardown is separate from the original persistent-task reset.
void ShutdownNativeDispatchTask();
}
