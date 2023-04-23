/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "PeerConnectDisconnectWithCancelPendingTest.h"

#include <chrono>
#include <thread>

/*
What is being done here is having 8 peers all connect to eachother, disconnect, connect again.

Do this for about 10 seconds. Then allow them all to connect for one last time.

This test also tests the CancelConnectionAttempt.

Also tests nonblocking connects, the simpler one PeerConnectDisconnect tests without it

Good ideas for changes:
After the last check run a eightpeers like test and add the conditions
of that test as well.

Make sure that if we initiate the connection we get a proper message
and if not we get a proper message. Add proper conditions.

Randomize sending the disconnect notes

Success conditions:
All connected normally and pending requests get canceled normally.

Failure conditions:
Doesn't reconnect normally.

During the very first connect loop any connect returns false.

Connect function returns false and peer is not connected to anything.

Pending request is not canceled.

*/
int PeerConnectDisconnectWithCancelPendingTest::RunTest( bool isVerbose, bool noPauses )
{
    const int peerNum = 8;
    const int maxConnections = peerNum * 3; //Max allowed connections for test set to times 3 to eliminate problem variables
    RakPeerInterface* peerList[peerNum];    //A list of 8 peers

    Packet* packet;
    destroyList.clear();

    //Initializations of the arrays
    for( int i = 0; i < peerNum; i++ )
    {
        peerList[i] = RakPeerInterface::GetInstance();
        destroyList.push_back( peerList[i] );

        peerList[i]->Startup( maxConnections, &SocketDescriptor( 60000 + i, 0 ), 1 );
        peerList[i]->SetMaximumIncomingConnections( maxConnections );
    }

    //Connect all the peers together

    for( int i = 0; i < peerNum; i++ )
    {

        for( int j = i + 1; j < peerNum; j++ ) //Start at i+1 so don't connect two of the same together.
        {

            if( peerList[i]->Connect( "127.0.0.1", 60000 + j, 0, 0 ) != CONNECTION_ATTEMPT_STARTED )
            {

                if( isVerbose )
                    DebugTools::ShowError( "Problem while calling connect.", !noPauses && isVerbose, __LINE__, __FILE__ );

                return 1; //This fails the test, don't bother going on.
            }
        }
    }

    TimeMS entryTime = GetTimeMS(); //Loop entry time

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    printf( "Entering disconnect loop \n" );
    bool printedYet;

    while( GetTimeMS() - entryTime < 10000 ) //Run for 10 Secoonds
    {
        //Disconnect all peers IF connected to any
        for( int i = 0; i < peerNum; i++ )
        {
            peerList[i]->GetSystemList( systemList, guidList ); //Get connectionlist

            for( const SystemAddress& rSystem : systemList ) //Disconnect them all
            {
                peerList[i]->CloseConnection( rSystem, true, 0, LOW_PRIORITY );
            }
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        //Clear pending if not finished

        for( int i = 0; i < peerNum; i++ )
        {

            for( int j = i + 1; j < peerNum; j++ ) //Start at i+1 so don't connect two of the same together.
            {
                SystemAddress currentSystem( "127.0.0.1", 60000 + j );

                peerList[i]->CancelConnectionAttempt( currentSystem ); //Make sure a connection is not pending before trying to connect.
            }
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        //Connect

        for( int i = 0; i < peerNum; i++ )
        {

            for( int j = i + 1; j < peerNum; j++ ) //Start at i+1 so don't connect two of the same together.
            {

                if( peerList[i]->Connect( "127.0.0.1", 60000 + j, 0, 0 ) != CONNECTION_ATTEMPT_STARTED )
                {
                    SystemAddress currentSystem( "127.0.0.1", 60000 + j );

                    peerList[i]->GetSystemList( systemList, guidList ); //Get connectionlist

                    if( CommonFunctions::ConnectionStateMatchesOptions( peerList[i], currentSystem, false, true, true ) ) //Did we drop the pending connection?
                    {
                        if( isVerbose )
                            DebugTools::ShowError( "Did not cancel the pending request \n", !noPauses && isVerbose, __LINE__, __FILE__ );

                        return 3;
                    }

                    if( !CommonFunctions::ConnectionStateMatchesOptions( peerList[i], currentSystem, true, true, true, true ) )
                    {
                        if( isVerbose )
                            DebugTools::ShowError( "Problem while calling connect. \n", !noPauses && isVerbose, __LINE__, __FILE__ );

                        return 1; //This fails the test, don't bother going on.
                    }
                }
            }
        }

        for( int i = 0; i < peerNum; i++ ) //Receive for all peers
        {

            printedYet = false;

            packet = peerList[i]->Receive();

            while( packet )
            {

                if( isVerbose && !printedYet )
                {
                    printf( "For peer %i\n", i );
                    printedYet = true;
                }
                switch( packet->data[0] )
                {
                case ID_REMOTE_DISCONNECTION_NOTIFICATION:
                    if( isVerbose )
                        printf( "Another client has disconnected.\n" );

                    break;
                case ID_REMOTE_CONNECTION_LOST:
                    if( isVerbose )
                        printf( "Another client has lost the connection.\n" );

                    break;
                case ID_REMOTE_NEW_INCOMING_CONNECTION:
                    if( isVerbose )
                        printf( "Another client has connected.\n" );
                    break;
                case ID_CONNECTION_REQUEST_ACCEPTED:
                    if( isVerbose )
                        printf( "Our connection request has been accepted.\n" );

                    break;
                case ID_CONNECTION_ATTEMPT_FAILED:
                    if( isVerbose )
                        printf( "A connection has failed.\n" );

                    break;

                case ID_NEW_INCOMING_CONNECTION:
                    if( isVerbose )
                        printf( "A connection is incoming.\n" );

                    break;
                case ID_NO_FREE_INCOMING_CONNECTIONS:
                    if( isVerbose )
                        printf( "The server is full.\n" );

                    break;

                case ID_ALREADY_CONNECTED:
                    if( isVerbose )
                        printf( "Already connected\n" );

                    break;

                case ID_DISCONNECTION_NOTIFICATION:
                    if( isVerbose )
                        printf( "We have been disconnected.\n" );
                    break;
                case ID_CONNECTION_LOST:
                    if( isVerbose )
                        printf( "Connection lost.\n" );

                    break;
                default:

                    break;
                }

                peerList[i]->DeallocatePacket( packet );

                // Stay in the loop as long as there are more packets.
                packet = peerList[i]->Receive();
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) ); //If needed for testing
    }

    while( GetTimeMS() - entryTime < 2000 ) //Run for 2 Secoonds to process incoming disconnects
    {

        for( int i = 0; i < peerNum; i++ ) //Receive for all peers
        {
            printedYet = false;

            packet = peerList[i]->Receive();

            while( packet )
            {

                if( isVerbose && !printedYet )
                {
                    printf( "For peer %i\n", i );
                    printedYet = true;
                }
                switch( packet->data[0] )
                {
                case ID_REMOTE_DISCONNECTION_NOTIFICATION:
                    if( isVerbose )
                        printf( "Another client has disconnected.\n" );

                    break;
                case ID_REMOTE_CONNECTION_LOST:
                    if( isVerbose )
                        printf( "Another client has lost the connection.\n" );

                    break;
                case ID_REMOTE_NEW_INCOMING_CONNECTION:
                    if( isVerbose )
                        printf( "Another client has connected.\n" );
                    break;
                case ID_CONNECTION_REQUEST_ACCEPTED:
                    if( isVerbose )
                        printf( "Our connection request has been accepted.\n" );

                    break;
                case ID_CONNECTION_ATTEMPT_FAILED:
                    if( isVerbose )
                        printf( "A connection has failed.\n" );

                    break;

                case ID_NEW_INCOMING_CONNECTION:
                    if( isVerbose )
                        printf( "A connection is incoming.\n" );

                    break;
                case ID_NO_FREE_INCOMING_CONNECTIONS:
                    if( isVerbose )
                        printf( "The server is full.\n" );

                    break;

                case ID_ALREADY_CONNECTED:
                    if( isVerbose )
                        printf( "Already connected\n" );

                    break;

                case ID_DISCONNECTION_NOTIFICATION:
                    if( isVerbose )
                        printf( "We have been disconnected.\n" );
                    break;
                case ID_CONNECTION_LOST:
                    if( isVerbose )
                        printf( "Connection lost.\n" );

                    break;
                default:

                    break;
                }

                peerList[i]->DeallocatePacket( packet );

                // Stay in the loop as long as there are more packets.
                packet = peerList[i]->Receive();
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) ); //If needed for testing
    }

    //Connect

    for( int i = 0; i < peerNum; i++ )
    {
        for( int j = i + 1; j < peerNum; j++ ) //Start at i+1 so don't connect two of the same together.
        {
            peerList[i]->CancelConnectionAttempt( SystemAddress( "127.0.0.1", 60000 + j ) ); //Make sure a connection is not pending before trying to connect.
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

        for( int j = i + 1; j < peerNum; j++ ) //Start at i+1 so don't connect two of the same together.
        {
            SystemAddress currentSystem( "127.0.0.1", 60000 + j );

            if( peerList[i]->Connect( "127.0.0.1", 60000 + j, 0, 0 ) != CONNECTION_ATTEMPT_STARTED )
            {
                peerList[i]->GetSystemList( systemList, guidList ); //Get connectionlist

                if( systemList.empty() ) //No connections, should not fail.
                {
                    if( isVerbose )
                        DebugTools::ShowError( "Problem while calling connect \n", !noPauses && isVerbose, __LINE__, __FILE__ );

                    return 1; //This fails the test, don't bother going on.
                }
            }
        }
    }

    entryTime = GetTimeMS();

    while( GetTimeMS() - entryTime < 5000 ) //Run for 5 Secoonds
    {

        for( int i = 0; i < peerNum; i++ ) //Receive for all peers
        {

            printedYet = false;
            packet = peerList[i]->Receive();

            while( packet )
            {
                if( isVerbose && !printedYet )
                {
                    printf( "For peer %i\n", i );
                    printedYet = true;
                }
                switch( packet->data[0] )
                {
                case ID_REMOTE_DISCONNECTION_NOTIFICATION:
                    if( isVerbose )
                        printf( "Another client has disconnected.\n" );

                    break;
                case ID_REMOTE_CONNECTION_LOST:
                    if( isVerbose )
                        printf( "Another client has lost the connection.\n" );

                    break;
                case ID_REMOTE_NEW_INCOMING_CONNECTION:
                    if( isVerbose )
                        printf( "Another client has connected.\n" );
                    break;
                case ID_CONNECTION_REQUEST_ACCEPTED:
                    if( isVerbose )
                        printf( "Our connection request has been accepted.\n" );

                    break;
                case ID_CONNECTION_ATTEMPT_FAILED:
                    if( isVerbose )
                        printf( "A connection has failed.\n" );

                    break;

                case ID_NEW_INCOMING_CONNECTION:
                    if( isVerbose )
                        printf( "A connection is incoming.\n" );

                    break;
                case ID_NO_FREE_INCOMING_CONNECTIONS:
                    if( isVerbose )
                        printf( "The server is full.\n" );

                    break;

                case ID_ALREADY_CONNECTED:
                    if( isVerbose )
                        printf( "Already connected\n" );

                    break;

                case ID_DISCONNECTION_NOTIFICATION:
                    if( isVerbose )
                        printf( "We have been disconnected.\n" );
                    break;
                case ID_CONNECTION_LOST:
                    if( isVerbose )
                        printf( "Connection lost.\n" );

                    break;
                default:

                    break;
                }

                peerList[i]->DeallocatePacket( packet );

                // Stay in the loop as long as there are more packets.
                packet = peerList[i]->Receive();
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) ); //If needed for testing
    }

    for( int i = 0; i < peerNum; i++ )
    {
        peerList[i]->GetSystemList( systemList, guidList );
        //Get the number of connections for the current peer
        if( guidList.size() != peerNum - 1 ) //Did we connect to all?
        {
            if( isVerbose )
                DebugTools::ShowError( "Not all peers reconnected normally.\n ", !noPauses && isVerbose, __LINE__, __FILE__ );

            return 2;
        }
    }

    if( isVerbose )
        printf( "Pass\n" );
    return 0;
}

std::string PeerConnectDisconnectWithCancelPendingTest::GetTestName() const
{
    return "PeerConnectDisconnectWithCancelPendingTest";
}

std::string PeerConnectDisconnectWithCancelPendingTest::ErrorCodeToString( int errorCode ) const
{
    // clang-format off
    switch( errorCode )
    {
    case  0: return "No error";                             break;
    case  1: return "The connect function failed.";         break;
    case  2: return "Peers did not connect normally.";      break;
    case  3: return "Pending connection was not canceled."; break;
    default: return "Undefined Error";                      break;
    }
    // clang-format on
}

PeerConnectDisconnectWithCancelPendingTest::PeerConnectDisconnectWithCancelPendingTest( void )
{
}

PeerConnectDisconnectWithCancelPendingTest::~PeerConnectDisconnectWithCancelPendingTest( void )
{
}

void PeerConnectDisconnectWithCancelPendingTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}
