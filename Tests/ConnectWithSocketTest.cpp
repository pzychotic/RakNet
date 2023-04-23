/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "ConnectWithSocketTest.h"

#include <chrono>
#include <thread>

/*
Description:
virtual ConnectionAttemptResult RakPeerInterface::ConnectWithSocket( const char* host, unsigned short remotePort, const char* passwordData, int passwordDataLength, RakNetSocket2* socket, PublicKey* publicKey=0, unsigned sendConnectionAttemptCount=12, unsigned timeBetweenSendConnectionAttemptsMS=500, RakNet::TimeMS timeoutTime=0 )
virtual void RakPeerInterface::GetSockets( std::vector<RakNetSocket2*>& sockets )
virtual RakNetSocket2* RakPeerInterface::GetSocket( const SystemAddress target )

Success conditions:

Failure conditions:

RakPeerInterface Functions used, tested indirectly by its use:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send
IsConnected

RakPeerInterface Functions Explicitly Tested:
ConnectWithSocket
GetSockets
GetSocket

*/
int ConnectWithSocketTest::RunTest( bool isVerbose, bool noPauses )
{
    destroyList.clear();

    RakPeerInterface *server, *client;

    TestHelpers::StandardClientPrep( client, destroyList );
    TestHelpers::StandardServerPrep( server, destroyList );

    SystemAddress serverAddress( "127.0.0.1", 60000 );

    printf( "Testing normal connect before test\n" );
    if( !TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[1 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 1;
    }

    TestHelpers::BroadCastTestPacket( client );

    if( !TestHelpers::WaitForTestPacket( server, 5000 ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[2 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 2;
    }

    printf( "Disconnecting client\n" );
    CommonFunctions::DisconnectAndWait( client, "127.0.0.1", 60000 );

    std::vector<RakNetSocket2*> sockets;
    client->GetSockets( sockets );

    RakNetSocket2* theSocket = sockets[0];

    RakTimer timer2( 5000 );

    printf( "Testing ConnectWithSocket using socket from GetSockets\n" );
    while( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true ) && !timer2.IsExpired() )
    {

        if( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true, true, true, true ) )
        {
            client->ConnectWithSocket( "127.0.0.1", serverAddress.GetPort(), 0, 0, theSocket );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    if( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[3 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 3;
    }

    TestHelpers::BroadCastTestPacket( client );

    if( !TestHelpers::WaitForTestPacket( server, 5000 ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[4 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 4;
    }

    printf( "Disconnecting client\n" );
    CommonFunctions::DisconnectAndWait( client, "127.0.0.1", 60000 );

    printf( "Testing ConnectWithSocket using socket from GetSocket\n" );
    theSocket = client->GetSocket( UNASSIGNED_SYSTEM_ADDRESS ); //Get open Socket

    timer2.Start();

    while( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true ) && !timer2.IsExpired() )
    {

        if( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true, true, true, true ) )
        {
            client->ConnectWithSocket( "127.0.0.1", serverAddress.GetPort(), 0, 0, theSocket );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    if( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[5 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 5;
    }

    TestHelpers::BroadCastTestPacket( client );

    if( !TestHelpers::WaitForTestPacket( server, 5000 ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[6 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 6;
    }

    return 0;
}

std::string ConnectWithSocketTest::GetTestName() const
{
    return "ConnectWithSocketTest";
}

std::string ConnectWithSocketTest::ErrorCodeToString( int errorCode ) const
{
    if( errorCode > 0 && (unsigned int)errorCode <= errorList.size() )
    {
        return errorList[errorCode - 1];
    }
    else
    {
        return "Undefined Error";
    }
}

ConnectWithSocketTest::ConnectWithSocketTest( void )
{
    errorList.emplace_back( "Client did not connect after 5 seconds" );
    errorList.emplace_back( "Control test send didn't work" );
    errorList.emplace_back( "Client did not connect after 5 secods Using ConnectWithSocket, could be GetSockets or ConnectWithSocket problem" );
    errorList.emplace_back( "Server did not recieve test packet from client" );
    errorList.emplace_back( "Client did not connect after 5 secods Using ConnectWithSocket, could be GetSocket or ConnectWithSocket problem" );
    errorList.emplace_back( "Server did not recieve test packet from client" );
}

ConnectWithSocketTest::~ConnectWithSocketTest( void )
{
}

void ConnectWithSocketTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}
