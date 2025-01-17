/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "CrossConnectionConvertTest.h"

#include <chrono>
#include <thread>

 /*
Description: Tests what happens if two instances of RakNet connect to each other at the same time. This has caused handshaking problems in the past.

Success conditions:
Everything connects and sends normally.

Failure conditions:
Expected values from ping/pong do not occur within expected time.
*/
int CrossConnectionConvertTest::RunTest( bool isVerbose, bool noPauses )
{
    static const unsigned short SERVER_PORT = 1234;
    //  char serverMode[32];
    char serverIP[64];

    strcpy( serverIP, "127.0.0.1" );

    char clientIP[64];
    RakPeerInterface *server, *client;
    unsigned short clientPort;
    bool gotNotification;

    destroyList.clear();
    server = RakPeerInterface::GetInstance();
    destroyList.push_back( server );
    client = RakPeerInterface::GetInstance();
    destroyList.push_back( client );


    server->Startup( 1, &SocketDescriptor( SERVER_PORT, 0 ), 1 );
    server->SetMaximumIncomingConnections( 1 );

    client->Startup( 1, &SocketDescriptor( 0, 0 ), 1 );

    client->Ping( serverIP, SERVER_PORT, false );

    //  PacketLogger pl;
    //  pl.LogHeader();
    //  rakPeer->AttachPlugin(&pl);

    TimeMS connectionAttemptTime = 0, connectionResultDeterminationTime = 0, nextTestStartTime = 0;

    TimeMS entryTime = GetTimeMS(); //Loop entry time

    bool printedYet = false;
    while( GetTimeMS() - entryTime < 10000 ) //Run for 10 Secoonds
    {

        Packet* p;

        printedYet = false;

        for( p = server->Receive(); p; server->DeallocatePacket( p ), p = server->Receive() )
        {

            if( isVerbose && !printedYet )
            {
                printf( "Server:\n" );
                printedYet = true;
            }
            if( p->data[0] == ID_NEW_INCOMING_CONNECTION )
            {

                if( isVerbose )
                    printf( "ID_NEW_INCOMING_CONNECTION\n" );
                gotNotification = true;
            }
            else if( p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED )
            {

                if( isVerbose )
                    printf( "ID_CONNECTION_REQUEST_ACCEPTED\n" );
                gotNotification = true;
            }
            else if( p->data[0] == ID_UNCONNECTED_PING )
            {

                if( isVerbose )
                    printf( "ID_PING\n" );
                connectionAttemptTime = GetTimeMS() + 1000;
                p->systemAddress.ToString( false, clientIP );
                clientPort = p->systemAddress.GetPort();
                gotNotification = false;
            }
            else if( p->data[0] == ID_UNCONNECTED_PONG )
            {

                if( isVerbose )
                    printf( "ID_UNCONNECTED_PONG\n" );
                TimeMS sendPingTime;
                BitStream bs( p->data, p->length, false );
                bs.IgnoreBytes( 1 );
                bs.Read( sendPingTime );
                TimeMS rtt = GetTimeMS() - sendPingTime;
                if( rtt / 2 <= 500 )
                    connectionAttemptTime = GetTimeMS() + 1000 - rtt / 2;
                else
                    connectionAttemptTime = GetTimeMS();
                gotNotification = false;
            }
        }

        printedYet = false;
        for( p = client->Receive(); p; client->DeallocatePacket( p ), p = client->Receive() )
        {

            if( isVerbose && !printedYet )
            {
                printf( "Client:\n" );
                printedYet = true;
            }
            if( p->data[0] == ID_NEW_INCOMING_CONNECTION )
            {

                if( isVerbose )
                    printf( "ID_NEW_INCOMING_CONNECTION\n" );
                gotNotification = true;
            }
            else if( p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED )
            {

                if( isVerbose )
                    printf( "ID_CONNECTION_REQUEST_ACCEPTED\n" );
                gotNotification = true;
            }
            else if( p->data[0] == ID_UNCONNECTED_PING )
            {

                if( isVerbose )
                    printf( "ID_PING\n" );
                connectionAttemptTime = GetTimeMS() + 1000;
                p->systemAddress.ToString( false, clientIP );
                clientPort = p->systemAddress.GetPort();
                gotNotification = false;
            }
            else if( p->data[0] == ID_UNCONNECTED_PONG )
            {

                if( isVerbose )
                    printf( "ID_UNCONNECTED_PONG\n" );
                TimeMS sendPingTime;
                BitStream bs( p->data, p->length, false );
                bs.IgnoreBytes( 1 );
                bs.Read( sendPingTime );
                TimeMS rtt = GetTimeMS() - sendPingTime;
                if( rtt / 2 <= 500 )
                    connectionAttemptTime = GetTimeMS() + 1000 - rtt / 2;
                else
                    connectionAttemptTime = GetTimeMS();
                gotNotification = false;
            }
        }

        if( connectionAttemptTime != 0 && GetTimeMS() >= connectionAttemptTime )
        {

            if( isVerbose )
                printf( "Attemping connection\n" );
            connectionAttemptTime = 0;

            server->Connect( clientIP, clientPort, 0, 0 );
            client->Connect( serverIP, SERVER_PORT, 0, 0 );

            connectionResultDeterminationTime = GetTimeMS() + 2000;
        }
        if( connectionResultDeterminationTime != 0 && GetTimeMS() >= connectionResultDeterminationTime )
        {
            connectionResultDeterminationTime = 0;
            if( gotNotification == false )
            {
                DebugTools::ShowError( "Did not recieve expected response. \n", !noPauses && isVerbose, __LINE__, __FILE__ );
                return 1;
            }

            client->CancelConnectionAttempt( SystemAddress( serverIP, SERVER_PORT ) );
            server->CancelConnectionAttempt( SystemAddress( clientIP, clientPort ) );

            server->CloseConnection( server->GetSystemAddressFromIndex( 0 ), true, 0 );
            client->CloseConnection( client->GetSystemAddressFromIndex( 0 ), true, 0 );

            //if (isServer==false)
            nextTestStartTime = GetTimeMS() + 1000;
        }
        if( nextTestStartTime != 0 && GetTimeMS() >= nextTestStartTime )
        {
            client->Ping( serverIP, SERVER_PORT, false );
            nextTestStartTime = 0;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }
    if( isVerbose )
        printf( "Test succeeded.\n" );

    return 0;
}

std::string CrossConnectionConvertTest::GetTestName() const
{
    return "CrossConnectionConvertTest";
}

std::string CrossConnectionConvertTest::ErrorCodeToString( int errorCode ) const
{
    // clang-format off
    switch( errorCode )
    {
    case  0: return "No error";                             break;
    case  1: return "Did not recieve expected response";    break;
    default: return "Undefined Error";                      break;
    }
    // clang-format on
}

void CrossConnectionConvertTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}

CrossConnectionConvertTest::CrossConnectionConvertTest( void )
{
}

CrossConnectionConvertTest::~CrossConnectionConvertTest( void )
{
}
