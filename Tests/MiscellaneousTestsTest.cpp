/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "MiscellaneousTestsTest.h"

/*
Description:
Tests:
virtual void    SetRouterInterface (RouterInterface *routerInterface)=0
virtual void    RemoveRouterInterface (RouterInterface *routerInterface)=0
virtual bool    AdvertiseSystem (const char *host, unsigned short remotePort, const char *data, int dataLength, unsigned connectionSocketIndex=0)=0

Success conditions:

Failure conditions:

RakPeerInterface Functions used, tested indirectly by its use,list may not be complete:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send

RakPeerInterface Functions Explicitly Tested:
SetRouterInterface
RemoveRouterInterface
AdvertiseSystem

*/
int MiscellaneousTestsTest::RunTest( bool isVerbose, bool noPauses )
{
    destroyList.clear();

    RakPeerInterface *client, *server;

    TestHelpers::StandardClientPrep( client, destroyList );
    TestHelpers::StandardServerPrep( server, destroyList );

    printf( "Testing AdvertiseSystem\n" );

    client->AdvertiseSystem( "127.0.0.1", 60000, 0, 0 );

    if( !CommonFunctions::WaitForMessageWithID( server, ID_ADVERTISE_SYSTEM, 5000 ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[1 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 1;
    }

    return 0;
}

std::string MiscellaneousTestsTest::GetTestName() const
{
    return "MiscellaneousTestsTest";
}

std::string MiscellaneousTestsTest::ErrorCodeToString( int errorCode ) const
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

void MiscellaneousTestsTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}

MiscellaneousTestsTest::MiscellaneousTestsTest( void )
{

    errorList.emplace_back( "Did not recieve client advertise" );
    errorList.emplace_back( "The router interface should not be called because no send has happened yet" );
    errorList.emplace_back( "Router failed to trigger on failed directed send" );
    errorList.emplace_back( "Router was not properly removed" );
}

MiscellaneousTestsTest::~MiscellaneousTestsTest( void )
{
}
