#pragma once

#include "platform/system.h"
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace mscharged
{
struct ResolvedLaunch;
class StartupStopped : public std::runtime_error
{
public:
    StartupStopped(const char* symbol, const char* reason)
        : std::runtime_error(std::string(symbol) + ": " + reason) {}
};

[[noreturn]] void MissingStartupService(const char* symbol, const char* reason);
void InitializeStartupOS();
void ResetStartupMemory();
std::string StartupMemorySummary();
std::string VerifyStartupBootLoading();
std::string VerifyStartupParticleResources();
// Returns 3 at an explicit unimplemented service, 1 for a startup error.
int RunGameStartup(int argc, char** argv, const std::filesystem::path& config);
int RunGameStartup(int argc, char** argv, const ResolvedLaunch& launch);
}
