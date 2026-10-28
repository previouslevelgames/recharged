// The network numbers of the performance overlay (LAT, DLY, RB), read for the
// host after a game frame. Read only: the game is unchanged.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The netplay snapshot belongs to the isolated original game module
#endif
#include "platform/netplay_snapshot.h"

#include "platform/rollback_state.h"

#include "Game/NetworkDebug.h"
#include "NL/plat/TransportConnection.h"

#include <algorithm>
#include <cmath>
#include <iterator>

extern "C" __attribute__((visibility("default"))) bool charged_netplay_snapshot(ChargedNetplaySnapshot* snapshot,
                                                                                unsigned size)
{
    if (!snapshot || size != sizeof(ChargedNetplaySnapshot)) return false;
    ChargedNetplaySnapshot s{};
    s.round_trip_ms = -1;
    // A network session is one with a connection to another machine: the
    // game's reliable transport pings each (LAN and online alike), and the
    // slowest is what a match waits for. Only its first mConnectionCount
    // slots hold connections; the game never clears the others.
    if (NetworkSessionBase* session = g_pNetworkSessionBase)
        if (NetworkSocket* socket = session->GetDirectSocket(); socket && socket->mReliableSocket.mInitialized)
        {
            ReliableSocket& transport = socket->mReliableSocket;
            const int count = std::clamp(transport.mConnectionCount, 0, static_cast<int>(std::size(transport.mConnections)));
            for (int n = 0; n < count; ++n)
                if (TransportConnection* connection = transport.mConnections[n]; connection && !connection->IsClosed())
                {
                    s.network = 1;
                    s.round_trip_ms = std::max(s.round_trip_ms, connection->mRoundTripTimeMS);
                }
        }
    int delay = 0, predicted = 0;
    if (mscharged::rollback::OverlayNumbers(delay, predicted))
    {
        s.rollback = 1;
        s.input_delay_ms = static_cast<int>(std::lround(delay * g_fFixedUpdateTick * 1000.0f));
        s.predicted_steps = predicted;
    }
    *snapshot = s;
    return true;
}
