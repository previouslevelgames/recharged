#if defined(MSCHARGED_GAME_MODULE)
#error The providers belong to the program, not to a game image
#endif
// Functions the game calls that neither the decomp sources nor the host define
// yet: a few SDK and library functions and a method of an unfinished online
// scene. A dynamically loaded game module leaves them unresolved until one is
// called, and the dynamic linker then ends the game ("symbol lookup error").
// A static game image (cmake/StaticGameImages.cmake) must link them, so each one
// ends the game the same way, with its name. Nothing here pretends to work.
// tools/link_static_game_image.py refuses a provider the game defines itself.
#include "platform/process_exit.h"
#include "platform/session_log.h"

#include <cstdio>
#include <string>

namespace {
[[noreturn]] void Missing(const char* name)
{
    std::fprintf(stderr, "symbol lookup error: undefined symbol: %s (not in this build of the game)\n", name);
    mscharged::platform::SessionLogLine(std::string("Game called ") + name + ", which this build does not have");
    std::fflush(nullptr);
    mscharged::platform::TerminateProcessNow(127);
}
}

#define CHARGED_MISSING_PROVIDER(name) \
    extern "C" [[noreturn]] void name() { Missing(#name); }

// TLS for the online libraries' HTTP: online play reaches its server over plain
// HTTP (decomp patch 0727), so nothing calls these.
CHARGED_MISSING_PROVIDER(SSLConnect)
CHARGED_MISSING_PROVIDER(SSLDoHandshake)
CHARGED_MISSING_PROVIDER(SSLNew)
CHARGED_MISSING_PROVIDER(SSLRead)
CHARGED_MISSING_PROVIDER(SSLSetClientCert)
CHARGED_MISSING_PROVIDER(SSLSetClientCertDefault)
CHARGED_MISSING_PROVIDER(SSLSetRootCA)
CHARGED_MISSING_PROVIDER(SSLSetRootCADefault)
CHARGED_MISSING_PROVIDER(SSLShutdown)
CHARGED_MISSING_PROVIDER(SSLWrite)
// Random Mii faces of the Mii database (RFL_MakeRandomFace.c is not in the game).
CHARGED_MISSING_PROVIDER(RFLi_MakeRandomFace)
// MSL's case-insensitive compare (MSL/file_io.c is not in the game).
CHARGED_MISSING_PROVIDER(__msl_strnicmp)
// SDK functions Aurora and the native SDK do not have.
CHARGED_MISSING_PROVIDER(GXAbortFrame)
CHARGED_MISSING_PROVIDER(GXPeekARGB)
CHARGED_MISSING_PROVIDER(GXReadDrawSync)
CHARGED_MISSING_PROVIDER(OSGetStackPointer)
CHARGED_MISSING_PROVIDER(__OSReadNandbootInfo)
CHARGED_MISSING_PROVIDER(__OSWriteNandbootInfo)
// A method the decomp declares but does not define yet (online scenes).
// NetworkSessionBase::~NetworkSessionBase()
CHARGED_MISSING_PROVIDER(_ZN18NetworkSessionBaseD2Ev)
