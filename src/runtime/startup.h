#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace mscharged
{
class StartupStopped : public std::runtime_error
{
public:
    StartupStopped(const char* symbol, const char* reason)
        : std::runtime_error(std::string(symbol) + ": " + reason) {}
};

[[noreturn]] void MissingStartupService(const char* symbol, const char* reason);
void SetStartupSystemLanguage(std::uint8_t language);
void InitializeStartupOS();
void ResetStartupMemory();
std::string StartupMemorySummary();
// Returns 3 at an explicit unimplemented service, 1 for a startup error.
int RunGameStartup(int argc, char** argv, const std::filesystem::path& config);
}
