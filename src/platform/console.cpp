#include "platform/console.h"

#include <cstdarg>
#include <cstdio>
#include <string_view>

namespace mscharged::platform
{
namespace
{
const char* LevelName(AuroraLogLevel level)
{
    switch (level)
    {
    case LOG_DEBUG: return "debug";
    case LOG_INFO: return "info";
    case LOG_WARNING: return "warning";
    case LOG_ERROR: return "error";
    case LOG_FATAL: return "fatal";
    }
    return "unknown";
}

// Info messages a player needs: controllers coming and going, memory card
// repairs and which graphics adapter runs the game.
bool KeepOnNormalConsole(std::string_view module, std::string_view message)
{
    return module == "aurora::gamepad" || module == "aurora::card"
        || (module == "aurora::gpu" && message.rfind("Graphics adapter information", 0) == 0);
}
}

void Trace(const char* format, ...)
{
    if (!VerboseConsole()) return;
    va_list arguments;
    va_start(arguments, format);
    std::vprintf(format, arguments);
    va_end(arguments);
}

void ConsoleLog(AuroraLogLevel level, const char* module, const char* message, unsigned int length)
{
    const std::string_view name = module ? module : "";
    const std::string_view text(message, length);
    if (level == LOG_INFO && !VerboseConsole() && !KeepOnNormalConsole(name, text)) return;
    std::fprintf(stderr, "[%s] [%.*s] %.*s\n", LevelName(level), int(name.size()), name.data(), int(text.size()),
        text.data());
}
}
