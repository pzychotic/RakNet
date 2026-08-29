#pragma once

#include "RakNetTime.h"
#include "RakNetTypes.h"

namespace RakNet {
class RakPeerInterface;
}

/*
 *  Bounded waits on a peer's connection state, plus Drain - the suite's
 *  receive-and-deallocate primitive, which any polling loop needs whether or
 *  not it is polling for a connection.
 *
 *  Split out of PeerScope deliberately: PeerScope is about ownership, this is
 *  about time.
 *
 *  READ THIS BEFORE USING IT. These functions wait for a connection request to
 *  *settle*, not to *succeed*. The polled predicate is
 *  CommonFunctions::ConnectionStateMatchesOptions( peer, addr, isConnected=false,
 *  isConnecting=true, isPending=true ), which loops only while the state is
 *  IS_CONNECTING or IS_PENDING. IS_CONNECTED ends the wait, and so does
 *  IS_NOT_CONNECTED - a peer whose connection attempt just failed satisfies this
 *  wait immediately. Reading these names as "wait until connected" is what made
 *  the suite's one flaky test flaky. If a test needs peers to actually be
 *  connected, it must assert that separately - WaitForConnectionCounts below is
 *  the wait that does it.
 *
 *  The bound is an absolute deadline, not a per-call budget. Handing each of 256
 *  clients its own budget makes the worst case 256 x budget, which bounds nothing.
 *  One deadline is computed per wait and threaded through every peer.
 */
namespace ConnectionWaits {

// Hang guard, not a tuning knob: it is not scaled by peer count, because
// scaling invites re-tuning it per test. Absurdly generous for a handful of
// peers, and that is the point - expiry means something is wedged, never that
// the machine was busy.
constexpr RakNet::TimeMS kSettleBudget = 60000;

// The count wait's own hang guard, tighter than the settle budget because the
// counts come good in one poll or not at all: the 256-client callers run 90 s,
// almost all of it building peers, and reach their counts inside the first poll.
// Not scaled by peer count either.
constexpr RakNet::TimeMS kConnectionCountBudget = 10000;

// The cancel wait's hang guard, and the one budget in this file with a CEILING
// as well as a floor. The floor is the usual argument: the removal happens one
// update cycle after the cancel is queued, so two seconds is two orders of
// magnitude of slack. The ceiling is the part worth reading twice - RakPeer drops
// an unanswered attempt from the queue itself once its own send attempts run out,
// which leaves exactly the state a cancel leaves, so a budget that outlasts the
// attempt turns the wait below into one that cannot tell a cancel from a
// timeout. Callers must give the attempt a lifetime well beyond this number; see
// the note on WaitForAttemptsToBeCancelled.
constexpr RakNet::TimeMS kCancelBudget = 2000;

// The disconnect wait's hang guard. Same floor argument as the cancel budget and
// no ceiling to argue against: the close is applied on the update thread one cycle
// after CloseConnection queues it, and the connection leaves DISCONNECT_ASAP on
// the first update after its outgoing data has drained - two loopback update
// cycles, tens of milliseconds. Measured, both disconnects in ConnectWithSocketTest
// left inside 31-64 ms, which is one or two polls of the interval below: the wait
// is bounded by how often it looks rather than by the network. Five seconds is
// nearly two orders of magnitude past that.
constexpr RakNet::TimeMS kDisconnectBudget = 5000;

constexpr RakNet::TimeMS kPollInterval = 30;

// Blocks while peer's request toward addr is IS_CONNECTING or IS_PENDING.
// FAILs at the deadline, naming the state it was stuck in; wrap an INFO around
// the call to say which peer it was.
void WaitForRequestToSettle( RakNet::RakPeerInterface* peer, RakNet::SystemAddress addr, RakNet::TimeMS deadline );

// The plural convenience: one deadline at now + kSettleBudget, shared by all
// count peers. For the many-clients-one-server shape, where every peer is
// waiting on the same server address.
void WaitForRequestsToSettle( RakNet::RakPeerInterface* const* peers, int count, RakNet::SystemAddress addr );

// The all-pairs shape: peers[i] toward 127.0.0.1:basePort + j for EVERY ordered
// pair i != j, under one deadline for the whole wait. For the peer-mesh tests,
// which bind consecutive ports and call Connect on every pair.
//
// Every ordered pair, deliberately, and not the upper triangle the tests
// hand-rolled. Two things follow from sweeping i outer, j inner:
//
//   - Pair ( i, j ) is attempted by the lower-numbered peer, so the initiating
//     side i -> j is polled before the receiving side j -> i.
//   - A settled i -> j means peer i is CONNECTED or the request failed, and
//     peer i reaches CONNECTED only after peer j has begun handling the
//     request. So by the time j -> i is polled, peer j already holds a remote
//     entry for i and the poll is meaningful rather than an instant
//     IS_NOT_CONNECTED exit.
//
// Which is the whole point: when this returns, every attempted connection has
// reached its final state on BOTH sides, so a GetSystemList taken next is a
// reading rather than a race. Polling peerList[i] toward j > i only - the upper
// triangle - polls peer 6 once and peer 7 never at eight peers, and peer 7 is the
// side that accepts every one of its connections. That was the suite's one known
// flake.
//
// Expiry names both ends: the peer that was stuck and the peer it was stuck on.
void WaitForAllPairsToSettle( RakNet::RakPeerInterface* const* peers, int count, unsigned short basePort );

// The assertion-based shape: blocks until EVERY peer reports expectedCount
// connections through GetSystemList, or the deadline passes - one deadline at
// now + kConnectionCountBudget for the whole wait, as above.
//
// The pair waits above wait for a request to SETTLE. This is the wait that says
// they SUCCEEDED, and it is what the warning at the top of this file points at.
//
// Use it INSTEAD OF a check loop written after a wait, not before one: the loop
// becomes the polled predicate, so the counts are read until they are meaningful
// instead of snapshotted once.
//
// Expiry FAILs listing EVERY peer's index and actual count, never the first
// mismatch alone - which peers are short is the diagnosis.
//
// Deliberately does not drain, like the pair waits: it reads state Receive()
// has no part in, and a wait that silently ate packets would surprise a caller
// that wanted them. A caller polling long enough to care drains around it - see
// Drain below.
void WaitForConnectionCounts( RakNet::RakPeerInterface* const* peers, int count, int expectedCount );

// The cancel shape: blocks while any of count peers still has an attempt toward
// addr sitting in RakPeer's requestedConnectionQueue - which is what
// GetConnectionState reports as IS_PENDING - under one deadline at
// now + kCancelBudget for the whole sweep.
//
// The companion to CancelConnectionAttempt, which only queues the address:
// RakPeer::HandleConnectionCancelQueue does the removal on the update thread, so
// a caller that reads the state on the line after cancelling reads it before the
// cancel has happened. This is that missing settle window, and it is the reason
// this is a wait rather than a read.
//
// Like the settle waits, it says WHETHER the attempt left the queue and nothing
// about WHY it left. Two things can take it out: the cancel, and RakPeer giving
// up on an unanswered attempt on its own. A caller asserting that the CANCEL is
// what did it therefore has two obligations, neither of which this function can
// discharge for it:
//
//   - give the attempt a lifetime well beyond kCancelBudget, via Connect's
//     sendConnectionAttemptCount and timeBetweenSendConnectionAttemptsMS, so
//     RakPeer cannot be the one that removes it inside the window; and
//   - assert the resulting state separately afterwards, which is the same split
//     the settle waits ask for at the top of this file.
//
// Expiry FAILs naming the peer and the port it was still queued toward. Worth
// keeping distinct from the caller's own assertion: "the budget ran out with the
// attempt still queued" and "the attempt left the queue and became something
// unexpected" are different diagnoses, and only this function can report the
// first.
//
// Deliberately does not drain, like every wait above, for the same reason - it
// reads state Receive() has no part in. Its caller drains before calling it: the
// wait is bounded at two seconds and its expiry ends the test, so unlike the
// count wait there is no way to poll here long enough for a queue to matter.
void WaitForAttemptsToBeCancelled( RakNet::RakPeerInterface* const* peers, int count, RakNet::SystemAddress addr );

// The disconnect shape: blocks while peer's connection toward addr is still
// CONNECTED, CONNECTING, PENDING or DISCONNECTING - that is, until the connection
// is gone by any route - under a deadline at now + kDisconnectBudget.
//
// The companion to CloseConnection, which the CALLER issues, ONCE, on the line
// before. The ONCE is what is load-bearing, not the split: a helper that issued
// the close once and then polled would be just as correct. THE CLOSE MUST NOT GO
// INSIDE THE POLL, and that is worth the paragraph below, because the obvious
// reading of why is wrong.
//
// An unbounded poll whose body re-issues the close:
//
//     while( state is connected/connecting/pending/disconnecting )
//         peer->CloseConnection( target, true, 0, LOW_PRIORITY );
//
// livelocks, and hung this suite roughly one run in four. CloseConnection with
// sendDisconnectionNotification pushes a BufferedCommandStruct carrying a
// RELIABLE_ORDERED ID_DISCONNECTION_NOTIFICATION and the mode DISCONNECT_ASAP
// ( RakPeer::NotifyAndFlagForShutdown ). The update thread drains that queue at the
// top of every RunUpdateCycle, sends each command and applies its mode
// ( RakPeer.cpp:4949 ), and only AFTER that loop finishes does it reach the check
// that ends a disconnect: connectMode is DISCONNECT_ASAP and
// ReliabilityLayer::IsOutgoingDataWaiting() is false ( RakPeer.cpp:5192 ).
//
// It is NOT that the re-issued closes keep the resend buffer full. They do not: once
// the FIRST close has been applied, SendImmediate refuses to build a send list for a
// system already in DISCONNECT_ASAP ( RakPeer.cpp:3775 ), so every close after it
// queues nothing at all. What they do is starve the drain loop. The update thread
// paces itself at ten milliseconds a cycle ( RakPeer.cpp:5619 ), and a caller
// spinning with no sleep pushes some forty thousand commands into that queue per
// cycle - so whether the loop at 4949 ever reaches its end, and lets the thread get
// as far as 5192, is a race between two threads running flat out. Lose it and the
// state cannot leave IS_DISCONNECTING, because the code that would take it out is
// never reached. Measured on the healthy path the loop still burned 1.0M-5.5M spins
// and 230-1140 ms per disconnect - it wins that race, slowly. On a hung run:
// 188M spins, 39 s, still climbing.
//
// Not a library defect: a caller issuing millions of commands a second to a thread
// that answers ten times a second is what no amount of library care survives.
//
// Expiry FAILs naming the port it was still attached to and the state it was
// stuck in - IS_DISCONNECTING, if the mechanism above ever comes back by another
// route. It does not name the peer, and a test that closes more than one
// connection should wrap an INFO around the call to say which, exactly as
// WaitForRequestToSettle's callers do.
//
// Deliberately does not drain, like every wait above. Its callers reconnect the
// same pair afterwards and one of them reads the ping table across the gap, so a
// wait that quietly ate their packets would be a worse surprise than the queue a
// sub-second bounded wait can accumulate.
void WaitForDisconnect( RakNet::RakPeerInterface* peer, RakNet::SystemAddress addr );

// Has the absolute deadline passed? TimeMS is uint32_t and wraps roughly every
// 49 days, so a deadline cannot be compared with a plain >=; the signed
// difference is wrap-safe for any interval shorter than ~24 days, which every
// budget in the suite is by orders of magnitude.
bool Expired( RakNet::TimeMS deadline );

// Receive-and-deallocate until the queue is empty. The suite's drain primitive
// rather than a companion to the waits above: a test that polls without draining
// grows its queues without bound, and that holds for any polling loop, not just
// one waiting on a connection. PingTestsTest polls 25 s for its ping table to
// turn over, asserts on no connection state at all, and needs this for exactly
// that reason.
void Drain( RakNet::RakPeerInterface* peer );

// Drain every peer in the array, in index order. Same justification as the
// singular, multiplied: a loop that polls without draining grows its queues
// without bound, and a loop polling an array of peers grows `count` queues at
// once rather than one. The churn tests call this hundreds of thousands of
// times over 256 clients, which is where skipping it stops being survivable.
//
// It takes the array and nothing else. A caller that also holds a server writes
// two lines:
//
//     ConnectionWaits::Drain( server );
//     ConnectionWaits::DrainAll( clientList, kClientNum );
//
// rather than getting an overload that takes both, because a server is not a
// distinct role in a drain - it is a peer with a queue, like every other, and
// only the caller's variable names say otherwise.
void DrainAll( RakNet::RakPeerInterface* const* peers, int count );

} // namespace ConnectionWaits
