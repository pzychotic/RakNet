#pragma once

#include "RakPeerInterface.h"

#include <vector>

/*
 *  Owns every RakPeerInterface a Catch2 test case creates, and destroys them in
 *  reverse creation order when it leaves scope - normally, or by exception.
 *
 *  A failing REQUIRE throws and unwinds straight out of the test body, so cleanup
 *  written at the bottom of the body never runs and the peer keeps its port bound
 *  - which poisons the next test in the batch
 *  (docs/research/test-suite-baseline.md).
 *
 *  Declared as a plain local, first line of the test body:
 *
 *      TEST_CASE( "...", "[network]" )
 *      {
 *          PeerScope peers;
 *          RakNet::RakPeerInterface* server = peers.Server();
 *          ...
 *      }
 *
 *  A local rather than a TEST_CASE_METHOD fixture: it costs one line, does not
 *  force every test into the fixture macro, and composes with a test that wants
 *  members of its own. A file-static would be the trap - it is not reconstructed
 *  per test case.
 *
 *  Create() hands back a raw, NON-owning pointer, deliberately: it drops straight
 *  into the `RakPeerInterface* peerList[8]` locals and `RakPeerInterface**` helper
 *  signatures the tests use throughout.
 *
 *  Startup failures are reported with Catch2 assertions from inside Server() and
 *  Client(), so the bind error is the failure you see rather than a mystery three
 *  assertions later.
 */
class PeerScope
{
public:
    PeerScope() = default;
    ~PeerScope();

    PeerScope( const PeerScope& ) = delete;
    PeerScope& operator=( const PeerScope& ) = delete;

    // A tracked, un-started peer, for tests that call Startup themselves with
    // non-standard arguments.
    RakNet::RakPeerInterface* Create();

    // A started peer with its incoming limit set to maxConnections, and Startup's
    // result asserted.
    RakNet::RakPeerInterface* Server( unsigned short port = 60000, unsigned int maxConnections = 1 );

    // The same, without the incoming limit. port 0 = ephemeral.
    RakNet::RakPeerInterface* Client( unsigned short port = 0, unsigned int maxConnections = 1 );

    // Destroy-and-recreate a client in place. Without it the scope holds a
    // dangling pointer and double-frees at exit.
    //
    // Exactly one test in the suite does this, and deallocating a live client is
    // the whole point of it: ManyClientsOneServerDeallocateBlockingTest. Started
    // rather than bare, and a client rather than any peer, because that sole
    // caller wants precisely what Client() gives. The peer keeps its position in
    // the reverse-order teardown.
    void ReplaceWithClient( RakNet::RakPeerInterface*& slot, unsigned short port = 0, unsigned int maxConnections = 1 );

private:
    // Startup plus the REQUIRE on its result, shared by Client() and
    // ReplaceWithClient().
    static void Start( RakNet::RakPeerInterface* peer, const char* caller, unsigned short port, unsigned int maxConnections );

    std::vector<RakNet::RakPeerInterface*> m_peers;
};
