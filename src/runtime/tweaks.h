#pragma once
#include <stdexcept>
#include <string>
#include <memory>
class TweakValueBase;
struct TweakPendingValue;
namespace mscharged
{
void* AllocatePendingTweak();
void RememberTweakValue(TweakValueBase* value, const char* category);
void ForgetTweakValue(TweakValueBase* value);
void* OwnTweakAllocation(void* value);
bool TakeOwnedTweakValue(TweakValueBase* value);
void CheckTweakInitialization(unsigned char push_state);
int TweakInteger(const char* text);
float TweakFloat(const char* text);
class OriginalTweaks
{
    bool live_ = false;
public:
    OriginalTweaks();
    ~OriginalTweaks();
    OriginalTweaks(const OriginalTweaks&) = delete;
    OriginalTweaks& operator=(const OriginalTweaks&) = delete;
};
std::string VerifyStartupTweaks();
}
