#pragma once

#include "Plugins/TelnetTransport.h"
#include "TCPInterface.h"

#include <cstddef>

/*
 *  A TelnetTransport whose protected state the Telnet tests can read.
 */
class InspectableTelnetTransport : public RakNet::TelnetTransport
{
public:
    // TelnetClient entries, one per remote address with a connection still counted open.
    size_t ClientCount() const { return remoteClients.size(); }

    // Connections TCPInterface holds active, whether or not their new or lost events
    // were drained.
    unsigned short ConnectionCount() const { return tcpInterface->GetConnectionCount(); }

    // The first client's cursor, or 0 with no client.
    unsigned CursorPosition() const { return remoteClients.empty() ? 0 : remoteClients.front()->cursorPosition; }
};
