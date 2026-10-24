#if defined(MSCHARGED_GAME_MODULE)
#error The providers belong to the program, not to a game image
#endif
// Functions the game calls that neither the decomp sources nor the host define
// yet: Nintendo's online libraries and a few methods of unfinished online
// scenes. A dynamically loaded game module leaves them unresolved until one is
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

// Nintendo Wi-Fi Connection (DWC): online play, friends and rankings.
CHARGED_MISSING_PROVIDER(DWC_Base64Decode)
CHARGED_MISSING_PROVIDER(DWC_CheckDirtyFlag)
CHARGED_MISSING_PROVIDER(DWC_CheckUserData)
CHARGED_MISSING_PROVIDER(DWC_ClearDirtyFlag)
CHARGED_MISSING_PROVIDER(DWC_ClearError)
CHARGED_MISSING_PROVIDER(DWC_CloseAllConnectionsHard)
CHARGED_MISSING_PROVIDER(DWC_CreateUserData)
CHARGED_MISSING_PROVIDER(DWC_GetDateTime)
CHARGED_MISSING_PROVIDER(DWC_GetFriendDataType)
CHARGED_MISSING_PROVIDER(DWC_GetIngamesnCheckResult)
CHARGED_MISSING_PROVIDER(DWC_GetLastErrorEx)
CHARGED_MISSING_PROVIDER(DWC_GetMOMinCompState)
CHARGED_MISSING_PROVIDER(DWC_GetMyAID)
CHARGED_MISSING_PROVIDER(DWC_GetNumConnectionHost)
CHARGED_MISSING_PROVIDER(DWC_Init)
CHARGED_MISSING_PROVIDER(DWC_InitFriendsMatch)
CHARGED_MISSING_PROVIDER(DWC_IsBuddyFriendData)
CHARGED_MISSING_PROVIDER(DWC_IsValidFriendData)
CHARGED_MISSING_PROVIDER(DWC_LoginAsync)
CHARGED_MISSING_PROVIDER(DWC_ProcessFriendsMatch)
CHARGED_MISSING_PROVIDER(DWC_ReportUserData)
CHARGED_MISSING_PROVIDER(DWC_RnkGetScoreAsync)
CHARGED_MISSING_PROVIDER(DWC_RnkProcess)
CHARGED_MISSING_PROVIDER(DWC_RnkPutScoreAsync)
CHARGED_MISSING_PROVIDER(DWC_RnkResGetRow)
CHARGED_MISSING_PROVIDER(DWC_RnkResGetRowCount)
CHARGED_MISSING_PROVIDER(DWC_RnkShutdown)
CHARGED_MISSING_PROVIDER(DWC_SendUnreliable)
CHARGED_MISSING_PROVIDER(DWC_SetBuddyFriendCallback)
CHARGED_MISSING_PROVIDER(DWC_SetFriendStatusCallback)
CHARGED_MISSING_PROVIDER(DWC_SetOwnStatusData)
CHARGED_MISSING_PROVIDER(DWC_SetReportLevel)
CHARGED_MISSING_PROVIDER(DWC_Shutdown)
CHARGED_MISSING_PROVIDER(DWC_ShutdownFriendsMatch)
CHARGED_MISSING_PROVIDER(DWC_UpdateServersAsync)
// NET (message digests for online messages).
CHARGED_MISSING_PROVIDER(NETGetMD5Interface)
CHARGED_MISSING_PROVIDER(NETHMACGetDigest)
CHARGED_MISSING_PROVIDER(NETHMACInit)
CHARGED_MISSING_PROVIDER(NETHMACUpdate)
// SDK functions Aurora and the native SDK do not have.
CHARGED_MISSING_PROVIDER(GXAbortFrame)
CHARGED_MISSING_PROVIDER(GXPeekARGB)
CHARGED_MISSING_PROVIDER(GXReadDrawSync)
CHARGED_MISSING_PROVIDER(OSGetStackPointer)
CHARGED_MISSING_PROVIDER(__OSReadNandbootInfo)
CHARGED_MISSING_PROVIDER(__OSWriteNandbootInfo)
// Methods the decomp declares but does not define yet (online scenes).
// NetworkSessionBase::~NetworkSessionBase()
CHARGED_MISSING_PROVIDER(_ZN18NetworkSessionBaseD2Ev)
// SHOnlineFriendsChooseSides::OnSidesChanged(NetMessageSidesChanged*)
CHARGED_MISSING_PROVIDER(_ZN26SHOnlineFriendsChooseSides14OnSidesChangedEP22NetMessageSidesChanged)
// OnlineConnectionQualityScene::OnCheckConnection(NetMessageCheckConnection*)
CHARGED_MISSING_PROVIDER(_ZN28OnlineConnectionQualityScene17OnCheckConnectionEP25NetMessageCheckConnection)
// OnlineConnectionQualityScene::OnConnectionDecision(NetMessageConnectionDecision*)
CHARGED_MISSING_PROVIDER(_ZN28OnlineConnectionQualityScene20OnConnectionDecisionEP28NetMessageConnectionDecision)
