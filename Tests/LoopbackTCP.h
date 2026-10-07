#pragma once

#include "RakNetTime.h"
#include "SocketIncludes.h"

#include <cstddef>
#include <functional>
#include <string>

/*
 *  What a TCPInterface, PacketizedTCP or TelnetTransport test needs beyond the
 *  interface under test: a bounded wait, a raw client socket on loopback, and a
 *  probe for whether a descriptor is a socket.
 *
 *  The client is a raw socket rather than a second TCPInterface so a test can
 *  make it misbehave: send a header announcing more than follows, stop reading,
 *  or reconnect from the port it used before.
 */
namespace LoopbackTCP {

// Loopback, so every wait is over as soon as the threads have been scheduled
// once. Generous so a loaded machine cannot turn a pass into a failure.
constexpr RakNet::TimeMS kWaitBudget = 5000;

// ConnectionWaits::WaitUntil under kWaitBudget unless told otherwise: calls
// condition until it returns true or budget ms have passed, and returns its last
// answer. A short budget with a condition that must stay false is a quiet-period
// check.
bool WaitFor( const std::function<bool()>& condition, RakNet::TimeMS budget = kWaitBudget );

// Whether descriptor is an open socket.
bool IsSocket( __TCPSOCKET__ descriptor );

/*
 *  A blocking TCP connection to 127.0.0.1:listenPort, owned for the scope it is
 *  declared in. A failed step FAILs naming it, after closing the socket.
 *
 *  It closes abortively - SO_LINGER with a zero timeout, so the peer sees a RST
 *  rather than a FIN - in Abort and in the destructor, so a failed REQUIRE does
 *  not leak it into later tests. The abortive close leaves no TIME_WAIT behind,
 *  which is what lets a reconnect bind the same clientPort again at once.
 */
class Client
{
public:
    // From clientPort, with SO_REUSEADDR, or from an ephemeral port if 0. A
    // nonzero bufferSize sets SO_SNDBUF and SO_RCVBUF, so a stall arrives after
    // little data rather than after whatever autotuned loopback buffers absorb.
    explicit Client( unsigned short listenPort, unsigned short clientPort = 0, int bufferSize = 0 );
    ~Client();

    Client( Client&& other ) noexcept;
    Client( const Client& ) = delete;
    Client& operator=( const Client& ) = delete;
    Client& operator=( Client&& ) = delete;

    __TCPSOCKET__ Socket() const { return descriptor; }

    // Sends every byte, looping over partial sends; REQUIREs that each send
    // makes progress.
    void SendAll( const char* data, size_t length );
    void SendAll( const std::string& text ) { SendAll( text.data(), text.size() ); }

    // Closes abortively now. Once closed, a second call does nothing.
    void Abort();

private:
    __TCPSOCKET__ descriptor;
};

} // namespace LoopbackTCP
