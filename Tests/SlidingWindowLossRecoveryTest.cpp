#include "CCRakNetSlidingWindow.h"
#include "MTUSize.h"
#include "RakNetDefines.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins how CCRakNetSlidingWindow's send window reacts to a loss and recovers from it.

A loss costs at most half the window. The receiver NAKs every datagram it finds missing,
so one burst lost to a full socket buffer arrives as dozens of NAKs, and they are one
congestion event: the slow-start threshold is halved once for them, and never below one
MTU, the same as for a resend.

After a loss the window grows by about one MTU per round trip, as TCP Reno's congestion
avoidance does: every ack adds MTU * MTU / cwnd.

Without a loss the window stops growing at what the resend buffer can hold, so it stays
open however many bytes a connection has acked.

Driven directly, the way ReliabilityLayer drives it: datagrams take sequence numbers
from GetAndIncrementNextDatagramSequenceNumber, every ack and NAK names one, and
GetTransmissionBandwidth with nothing unacknowledged reads back the window.

Tagged [congestion]: nothing here binds a socket or creates a peer.
*/

using namespace RakNet;

namespace {

constexpr uint32_t kMtu = MAXIMUM_MTU_SIZE;
constexpr CCTimeType kRtt = 10000;

/// A sender's view of one CCRakNetSlidingWindow that always has more to send.
class Sender
{
public:
    Sender() { m_window.Init( 0, kMtu ); }

    /// The send window, in bytes.
    int Window() { return m_window.GetTransmissionBandwidth( m_now, 0, 0, true ); }

    /// Sends \a count datagrams and returns the sequence number of the first.
    DatagramSequenceNumberType Send( int count )
    {
        const DatagramSequenceNumberType first = m_window.GetNextDatagramSequenceNumber();
        for( int i = 0; i < count; ++i )
        {
            m_window.GetAndIncrementNextDatagramSequenceNumber();
        }
        return first;
    }

    /// One round trip: sends a full window of datagrams, and every one is acked. Returns how
    /// many were sent.
    int RoundTrip()
    {
        const int count = Window() / (int)kMtu;
        const DatagramSequenceNumberType first = Send( count );
        m_now += kRtt;
        for( int i = 0; i < count; ++i )
        {
            m_window.OnAck( m_now, kRtt, false, 0, 0, 0, true, first + (uint32_t)i );
        }
        return count;
    }

    /// NAKs \a count datagrams from \a first on, as a receiver does for a lost burst.
    void Nak( DatagramSequenceNumberType first, int count )
    {
        m_now += kRtt;
        for( int i = 0; i < count; ++i )
        {
            m_window.OnNAK( m_now, first + (uint32_t)i );
        }
    }

    void ResendTimesOut() { m_window.OnResend( m_now, m_now ); }

private:
    CCRakNetSlidingWindow m_window;
    CCTimeType m_now = 1000000;
};

/// Slow start from one MTU until the window is at least \a bytes.
void GrowTo( Sender& sender, int bytes )
{
    while( sender.Window() < bytes )
    {
        sender.RoundTrip();
    }
}

} // namespace

TEST_CASE( "NAKs arriving after a resend for the same lost burst cost nothing more", "[congestion]" )
{
    Sender sender;
    GrowTo( sender, 64 * (int)kMtu );
    const int before = sender.Window();

    const DatagramSequenceNumberType lost = sender.Send( 40 );
    sender.ResendTimesOut();
    sender.RoundTrip();
    sender.RoundTrip();
    sender.Nak( lost, 40 );
    for( int i = 0; i < 8; ++i )
    {
        sender.RoundTrip();
    }

    INFO( "window before the loss " << before << ", after " << sender.Window() );
    CHECK( sender.Window() >= before / 2 );
}

TEST_CASE( "A burst of NAKs from one loss costs at most half the window", "[congestion]" )
{
    Sender sender;
    GrowTo( sender, 64 * (int)kMtu );
    const int before = sender.Window();

    sender.Nak( sender.Send( 40 ), 40 );
    sender.ResendTimesOut();
    for( int i = 0; i < 8; ++i )
    {
        sender.RoundTrip();
    }

    INFO( "window before the loss " << before << ", after " << sender.Window() );
    CHECK( sender.Window() >= before / 2 );
}

TEST_CASE( "After a loss the window grows by about one MTU per round trip", "[congestion]" )
{
    Sender sender;
    GrowTo( sender, 128 * (int)kMtu );
    sender.Nak( sender.Send( 1 ), 1 );
    sender.RoundTrip();
    const int start = sender.Window();

    constexpr int kRoundTrips = 20;
    for( int i = 0; i < kRoundTrips; ++i )
    {
        sender.RoundTrip();
    }

    const int grownMtus = ( sender.Window() - start ) / (int)kMtu;
    INFO( "window grew from " << start << " to " << sender.Window() << " bytes" );
    CHECK( grownMtus >= kRoundTrips - 2 );
    CHECK( grownMtus <= kRoundTrips + 2 );
}

TEST_CASE( "A window that never loses a datagram stays open", "[congestion]" )
{
    Sender sender;

    // Every ack in slow start adds an MTU, so this many acks without a loss would take an
    // unbounded window past INT_MAX bytes.
    constexpr int64_t kAcks = ( int64_t( 1 ) << 31 ) / kMtu + 1;
    int64_t acked = 0;
    while( acked < kAcks )
    {
        const int sent = sender.RoundTrip();
        if( sent <= 0 )
        {
            break;
        }
        acked += sent;
    }

    INFO( "window " << sender.Window() << " bytes after " << acked << " acks" );
    CHECK( sender.Window() > 0 );
    CHECK( sender.Window() <= RESEND_BUFFER_ARRAY_LENGTH * (int)kMtu );
}
