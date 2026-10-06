#pragma once
#if !defined(MSCHARGED_DIAGNOSTIC_CONFIG) || defined(MSCHARGED_GAME_MODULE)
#error Configuration owners and validation helpers belong only to explicit diagnostics
#endif
#include "NL/nlConfig.h"
#include "NL/nlFunction.inl"
#include <memory>
#include <string>
#include <exception>

namespace mscharged
{
struct FreeConfigBuffer { void operator()(void* buffer) const noexcept; };
using ConfigBuffer = std::unique_ptr<void, FreeConfigBuffer>;

// A rejected replacement must retain both the previous value and string space.
class ConfigMutation
{
    Config& owner_;
    Config::TagValuePair& pair_;
    Config::TagValuePair previous_;
    char* end_;
    int exceptions_;
public:
    ConfigMutation(Config& owner, Config::TagValuePair& pair)
        : owner_(owner), pair_(pair), previous_(pair), end_(owner.mStringEnd), exceptions_(std::uncaught_exceptions()) {}
    ~ConfigMutation()
    {
        if (std::uncaught_exceptions() > exceptions_) { pair_ = previous_; owner_.mStringEnd = end_; }
    }
};

// Own the original global configuration within the initialized game arenas.
// Configuration and its callbacks are confined to the NL servicing thread.
class OriginalConfig
{
    std::unique_ptr<Config> config_;
public:
    OriginalConfig();
    ~OriginalConfig();
    OriginalConfig(const OriginalConfig&) = delete;
    OriginalConfig& operator=(const OriginalConfig&) = delete;
};
Config& GlobalGameConfig();
void ValidateConfigInput(const char* data, int size);
void LoadGameConfig(Config& config, const char* filename);
void LoadGameConfigAsync(Config& config, const char* filename, const Function<Config*>& callback);
void CancelConfigLoad(Config& config);
std::string VerifyStartupConfig();
}
