/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "SystemAddressAndGuidTest.h"

/*
Description:
Tests:
virtual unsigned short RakPeerInterface::NumberOfConnections    (   void         )       const
virtual void RakPeerInterface::GetSystemList    (   std::vector< SystemAddress > &      addresses,         std::vector< RakNetGUID > &    guids       )
virtual bool RakPeerInterface::IsActive     (   void         )       const
virtual SystemAddress RakPeerInterface::GetSystemAddressFromIndex   (   int      index       )
virtual SystemAddress RakPeerInterface::GetSystemAddressFromGuid    (   const RakNetGUID     input       )       const
virtual const RakNetGUID& RakPeerInterface::GetGuidFromSystemAddress    (   const SystemAddress      input       )       const
pure virtual  virtual RakNetGUID RakPeerInterface::GetGUIDFromIndex     (   int      index       )
virtual SystemAddress RakPeerInterface::GetExternalID   (   const SystemAddress      target      )       const

Success conditions:
All functions pass test.

Failure conditions:
Any function fails.

Client was active but shouldn't be yet
Client was not active but should be
Could not connect the client
Mismatch between guidList size and systemList size
NumberOfConnections problem
SystemList problem with GetSystemList
Both SystemList and Number of connections have problems and report different results
Both SystemList and Number of connections have problems and report same results
Undefined Error
System address from list is wrong.
Guid from list is wrong
GetSystemAddressFromIndex failed to return correct values
GetSystemAddressFromGuid failed to return correct values
GetGuidFromSystemAddress failed to return correct values
GetGUIDFromIndex failed to return correct values
GetExternalID failed to return correct values

RakPeerInterface Functions used, tested indirectly by its use. List may not be complete:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send
IsConnected

RakPeerInterface Functions Explicitly Tested:

NumberOfConnections
GetSystemList
IsActive
GetSystemAddressFromIndex
GetSystemAddressFromGuid
GetGuidFromSystemAddress
GetGUIDFromIndex
GetExternalID

*/
int SystemAddressAndGuidTest::RunTest( bool isVerbose, bool noPauses )
{
    RakPeerInterface *server, *client;
    destroyList.clear();

    printf( "Testing IsActive\n" );
    client = RakPeerInterface::GetInstance();
    destroyList.push_back( client );
    if( client->IsActive() )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[1 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 1;
    }

    client->Startup( 1, &SocketDescriptor( 60001, 0 ), 1 );

    if( !client->IsActive() )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[2 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 2;
    }

    //Passed by reference for initializations
    TestHelpers::StandardServerPrep( server, destroyList );

    if( !TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[3 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 3;
    }

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    printf( "Test GetSystemList and NumberOfConnections\n" );

    client->GetSystemList( systemList, guidList ); //Get connectionlist
    int len  = static_cast<int>( systemList.size() );
    int len2 = static_cast<int>( guidList.size() );

    int conNum = client->NumberOfConnections();

    printf( "Test if systemList size matches guidList size \n" );

    if( len2 != len )
    {

        printf( "system list size is %i and guid size is %i ", len, len2 );

        if( isVerbose )
            DebugTools::ShowError( errorList[4 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 4;
    }

    printf( "Test returned list size against NumberofConnections return value\n" );
    if( conNum != len )
    {

        if( conNum == 1 || len == 1 )
        {

            if( conNum != 1 )
            {
                printf( "system list size is %i and NumberOfConnections return is %i ", len, conNum );

                if( isVerbose )
                    DebugTools::ShowError( errorList[5 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

                return 5;
            }

            if( len != 1 )
            {

                printf( "system list size is %i and NumberOfConnections return is %i ", len, conNum );

                if( isVerbose )
                    DebugTools::ShowError( errorList[6 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

                return 6;
            }
        }
        else
        {
            printf( "system list size is %i and NumberOfConnections return is %i ", len, conNum );

            if( isVerbose )
                DebugTools::ShowError( errorList[7 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

            return 7;
        }
    }
    else
    {

        if( conNum != 1 )
        {
            printf( "system list size is %i and NumberOfConnections return is %i ", len, conNum );

            if( isVerbose )
                DebugTools::ShowError( errorList[8 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

            return 8;
        }
    }

    printf( "Test GetSystemListValues of the system and guid list\n" );
    SystemAddress serverAddress( "127.0.0.1", 60000 );

    if( !compareSystemAddresses( systemList[0], serverAddress ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[10 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 10;
    }

    RakNetGUID serverGuid = server->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS );

    if( guidList[0] != serverGuid )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[11 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 11;
    }

    printf( "Test GetSystemAddressFromIndex\n" );
    if( !compareSystemAddresses( client->GetSystemAddressFromIndex( 0 ), serverAddress ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[12 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 12;
    }

    printf( "Test GetSystemAddressFromGuid\n" );
    if( !compareSystemAddresses( client->GetSystemAddressFromGuid( serverGuid ), serverAddress ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[13 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 13;
    }

    printf( "Test GetGuidFromSystemAddress\n" );
    if( client->GetGuidFromSystemAddress( serverAddress ) != serverGuid )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[14 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 14;
    }

    printf( "Test GetGUIDFromIndex\n" );
    if( client->GetGUIDFromIndex( 0 ) != serverGuid )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[15 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 15;
    }

    SystemAddress clientAddress( "127.0.0.1", 60001 );

    printf( "Test GetExternalID, automatic testing is not only required for this\nbecause of it's nature\nShould be supplemented by internet tests\n" );

    if( !compareSystemAddresses( client->GetExternalID( serverAddress ), clientAddress ) )
    {

        if( isVerbose )
            DebugTools::ShowError( errorList[16 - 1], !noPauses && isVerbose, __LINE__, __FILE__ );

        return 16;
    }


    return 0;
}

std::string SystemAddressAndGuidTest::GetTestName() const
{
    return "SystemAddressAndGuidTest";
}

std::string SystemAddressAndGuidTest::ErrorCodeToString( int errorCode ) const
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

bool SystemAddressAndGuidTest::compareSystemAddresses( SystemAddress ad1, SystemAddress ad2 )
{
    if( ad1 != ad2 )
    {
        return 0;
    }

    return 1;
}

SystemAddressAndGuidTest::SystemAddressAndGuidTest( void )
{

    errorList.emplace_back( "Client was active but shouldn't be yet" );
    errorList.emplace_back( "Client was not active but should be" );
    errorList.emplace_back( "Could not connect the client" );
    errorList.emplace_back( "Mismatch between guidList size and systemList size " );
    errorList.emplace_back( "NumberOfConnections problem" );
    errorList.emplace_back( "SystemList problem with GetSystemList" );
    errorList.emplace_back( "Both SystemList and Number of connections have problems and report different results" );
    errorList.emplace_back( "Both SystemList and Number of connections have problems and report same results" );
    errorList.emplace_back( "Undefined Error" );
    errorList.emplace_back( "System address from list is wrong." );
    errorList.emplace_back( "Guid from list is wrong" );
    errorList.emplace_back( "GetSystemAddressFromIndex failed to return correct values" );
    errorList.emplace_back( "GetSystemAddressFromGuid failed to return correct values" );
    errorList.emplace_back( "GetGuidFromSystemAddress failed to return correct values" );
    errorList.emplace_back( "GetGUIDFromIndex failed to return correct values" );
    errorList.emplace_back( "GetExternalID failed to return correct values" );
}

SystemAddressAndGuidTest::~SystemAddressAndGuidTest( void )
{
}

void SystemAddressAndGuidTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}
