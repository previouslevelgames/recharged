#include "runtime/game_config.h"
#include "runtime/startup.h"
#include "NL/MemAlloc.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class E = std::exception, class F> void Reject(F action)
{
    try { action(); } catch (const E&) { ++checks; return; }
    throw std::runtime_error("Invalid configuration operation accepted");
}
void Run()
{
    Reject([] { Config::Global(); });
    mscharged::OriginalConfig session;
    Reject([] { mscharged::OriginalConfig duplicate; });
    Config& config = Config::Global();
    const char text[] = "# original comment\r\n[Video]\r\nCount=42\nScale = 1.25\nEnabled=ON\nName=\"Charged\" # tail\nDisabled=false\n";
    config.LoadFromBuffer(text, sizeof(text)-1);
    Check(config.mLoaded && config.Get<int>("video/count", 0) == 42, "Original integer or section parsing failed");
    Check(config.Get<float>("VIDEO/scale", 0) == 1.25f, "Original float parsing failed");
    Check(config.Get<bool>("video/enabled", false), "Original boolean parsing failed");
    Check(strcmp(config.Get<const char*>("video/name", ""), "Charged") == 0, "Original string parsing failed");
    Check(!config.Get<bool>("video/disabled", true), "Original false lookup failed");
    Check(config.FindTvp("video/disabled").type == CONFIG_STRING, "Original false-token classification changed");
    Check(config.Get<int>("missing", 7) == 7 && config.Exists("MISSING"), "Original default insertion changed");
    Reject<std::invalid_argument>([&] { config.LoadFromBuffer(nullptr, 1); });
    Reject<std::invalid_argument>([&] { config.LoadFromBuffer("a", -1); });
    Reject<std::invalid_argument>([&] { config.LoadFromBuffer("a\0b", 3); });
    std::string longline(255, 'a');
    Reject<std::length_error>([&] { config.LoadFromBuffer(longline.data(), longline.size()); });
    Check(!config.mLoaded, "Failed parse claimed completion");
    Reject<std::out_of_range>([&] { config.Set("overflow", "2147483648"); });
    config.Set("huge", 1e30f);
    Reject<std::out_of_range>([&] { config.Get<int>("huge", 0); });
    Config small(Config::ALLOCATE_LOW, 32, 1);
    small.Set("one", 1);
    Reject<std::length_error>([&] { small.Set("two", 2); });
    Check(!small.Exists("two") && small.Get<int>("one", 0) == 1, "Full table damaged existing entries");
    Config strings(Config::ALLOCATE_HIGH, 4, 4);
    strings.Set("abc", 1);
    Reject<std::length_error>([&] { strings.Set("abc", "replacement"); });
    Check(strings.Get<int>("abc",0)==1,"Rejected replacement corrupted the existing typed value");
    Reject<std::length_error>([&] { strings.Set("x", "text"); });
    Check(strings.Get<int>("ABC", 0) == 1, "String exhaustion damaged existing tag");
    Config::String original("abcdef"), shared(original);
    shared.erase(shared.c_str()+1, shared.c_str()+3);
    Check(original == "abcdef" && shared == "adef", "Copy-on-write erase used stale storage");
    Config::String empty;
    empty.insert(empty.begin(), original.c_str(), original.c_str()+6);
    Check(empty == "abcdef", "Empty string insertion failed");
    auto* at = empty.begin()+3;
    empty.insert(at, empty.c_str(), empty.c_str()+3);
    Check(empty == "abcabcdef", "Self insertion lost its source");
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024), mem2(1024*1024);
        mscharged::ResetStartupMemory();
        StandardAllocator.Initialize(mem1.data(), mem1.size()*8);
        VirtualAllocator.Initialize(mem2.data(), mem2.size()*8); gMemoryInitialized=1;
        for (int i=0; i<3; ++i)
        {
            Run();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,
                  "Configuration leaked game memory");
        }
        mscharged::ResetStartupMemory();
        std::cout << checks << " original configuration/string checks and three arena recoveries passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
