/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "MaximumConnectTest.h"

#include <chrono>
#include <thread>

/*
What is being done here is having 8 peers all connect to eachother over the max defined connection.

It runs the connect, wait 20 seconds then see the current connections.

Success conditions:
All extra connections Refused.

Failure conditions:
There are more connected than allowed.
The connect function fails, the test is not even done.
GetMaximumIncomingConnections returns wrong value.

RakPeerInterface Functions used, tested indirectly by its use:
Startup
Connect
SetMaximumIncomingConnections
Receive
DeallocatePacket
GetSystemList

RakPeerInterface Functions Explicitly Tested:
SetMaximumIncomingConnections
GetMaximumIncomingConnections

*/
int MaximumConnectTest::RunTest( bool isVerbose, bool noPauses )
{
    const int peerNum = 8;
    const int maxConnections = 4;        //Max allowed connections for test
    RakPeerInterface* peerList[peerNum]; //A list of 8 peers

    Packet* packet;
    destroyList.clear();

    int connReturn;
    //Initializations of the arrays
    for( int i = 0; i < peerNum; i++ )
    {
        peerList[i] = RakPeerInterface::GetInstance();
        destroyList.push_back( peerList[i] );

        peerList[i]->Startup( maxConnections, &SocketDescriptor( 60000 + i, 0 ), 1 );
        peerList[i]->SetMaximumIncomingConnections( maxConnections );

        connReturn = peerList[i]->GetMaximumIncomingConnections();
        if( connReturn != maxConnections )
        {
            if( isVerbose )
            {
                printf( "Getmaxconnections wrong for peer %i, %i should be the value but the value is %i.Fail\n", i, maxConnections, connReturn );

                DebugTools::ShowError( "", !noPauses && isVerbose, __LINE__, __FILE__ );
            }
        }
    }

    //Connect all the peers together

    for( int i = 0; i < peerNum; i++ )
    {

        for( int j = i + 1; j < peerNum; j++ ) //Start at i+1 so don't connect two of the same together.
        {

            if( peerList[i]->Connect( "127.0.0.1", 60000 + j, 0, 0 ) != CONNECTION_ATTEMPT_STARTED )
            {

                if( isVerbose )
                    DebugTools::ShowError( "Problem while calling connect.\n", !noPauses && isVerbose, __LINE__, __FILE__ );

                return 1; //This fails the test, don't bother going on.
            }
        }
    }

    TimeMS entryTime = GetTimeMS(); //Loop entry time

    while( GetTimeMS() - entryTime < 20000 ) //Run for 20 Secoonds
    {

        for( int i = 0; i < peerNum; i++ ) //Receive for all peers
        {

            packet = peerList[i]->Receive();

            if( isVerbose && packet )
                printf( "For peer %i\n", i );

            while( packet )
            {
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
                        printf( "A connection has failed.\n" ); //Should happen in this test

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
                        printf( "Already connected\n" ); //Shouldn't happen

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

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    for( int i = 0; i < peerNum; i++ )
    {
        peerList[i]->GetSystemList( systemList, guidList );

        int connNum = static_cast<int>( guidList.size() ); //Get the number of connections for the current peer
        if( connNum > maxConnections ) //Did we connect to more?
        {
            if( isVerbose )
            {
                printf( "More connections were allowed to peer %i, %i total.Fail\n", i, connNum );

                DebugTools::ShowError( "", !noPauses && isVerbose, __LINE__, __FILE__ );
            }

            return 2;
        }
    }

    if( isVerbose )
        printf( "Pass\n" );
    return 0;
}

std::string MaximumConnectTest::GetTestName() const
{
    return "MaximumConnectTest";
}

std::string MaximumConnectTest::ErrorCodeToString( int errorCode ) const
{
    // clang-format off
    switch( errorCode )
    {
    case  0: return "No error";                                             break;
    case  1: return "The connect function failed";                          break;
    case  2: return "An extra connection was allowed";                      break;
    case  3: return "GetMaximumIncomingConnectionsn returned wrong value";  break;
    default: return "Undefined Error";                                      break;
    }
    // clang-format on
}

MaximumConnectTest::MaximumConnectTest( void )
{
}

MaximumConnectTest::~MaximumConnectTest( void )
{
}

void MaximumConnectTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}
