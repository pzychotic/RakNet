/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "IncludeAllTests.h"

#include <vector>

using namespace RakNet;
/*

The TestInterface used in this was made to be able to be flexible
when adding new test cases. While I do not use the paramslist, it is available.

*/

int main( int argc, char* argv[] )
{
    std::vector<TestInterface*> testList; //Pointer list
    std::vector<int> testResultList;      //A list of pass and/or fail and the error codes
    std::vector<std::string> testsToRun;  //A list of tests to run
    std::vector<int> testsToRunIndexes;   //A list of tests to run by index

    //Add all the tests to the test list
    testList.push_back( new EightPeerTest() );
    testList.push_back( new MaximumConnectTest() );
    testList.push_back( new PeerConnectDisconnectWithCancelPendingTest() );
    testList.push_back( new PeerConnectDisconnectTest() );
    testList.push_back( new ManyClientsOneServerBlockingTest() );
    testList.push_back( new ManyClientsOneServerNonBlockingTest() );
    testList.push_back( new ManyClientsOneServerDeallocateBlockingTest() );
    testList.push_back( new ReliableOrderedConvertedTest() );
    testList.push_back( new DroppedConnectionConvertTest() );
    testList.push_back( new ComprehensiveConvertTest() );
    testList.push_back( new CrossConnectionConvertTest() );
    testList.push_back( new PingTestsTest() );
    testList.push_back( new OfflineMessagesConvertTest() );
    testList.push_back( new LocalIsConnectedTest() );
    testList.push_back( new SecurityFunctionsTest() );
    testList.push_back( new ConnectWithSocketTest() );
    testList.push_back( new SystemAddressAndGuidTest() );
    testList.push_back( new PacketAndLowLevelTestsTest() );
    testList.push_back( new MiscellaneousTestsTest() );

    int testListSize = static_cast<int>( testList.size() );

    bool isVerbose = true;
    bool disallowTestToPause = false;

    if( argc > 1 ) //we have command line arguments
    {
        for( int p = 1; p < argc; p++ )
        {
            testsToRun.push_back( argv[p] );
        }
    }

    int numTests = 0;
    int passedTests = 0;

    if( testsToRun.empty() && testsToRunIndexes.empty() )
    {
        numTests = testListSize;

        for( TestInterface* pTest : testList )
        {
            printf( "\n\nRunning test %s.\n\n", pTest->GetTestName().c_str() );
            int returnVal = pTest->RunTest( isVerbose, disallowTestToPause );
            pTest->DestroyPeers();

            if( returnVal == 0 )
            {
                passedTests++;
            }
            else
            {
                printf( "Test %s returned with error %s", pTest->GetTestName().c_str(), pTest->ErrorCodeToString( returnVal ).c_str() );
            }
        }
    }

    if( !testsToRun.empty() ) //If string arguments convert to indexes.Suggestion: if speed becoms an issue keep a sorted list and binary search it
    {
        for( const std::string& testName : testsToRun )
        {
            for( int j = 0; j < testListSize; j++ )
            {
                if( testList[j]->GetTestName() == testName )
                {
                    testsToRunIndexes.push_back( j );
                }
            }
        }
    }

    if( !testsToRunIndexes.empty() ) //Run selected indexes
    {
        numTests = static_cast<int>( testsToRunIndexes.size() );

        for( int iTestIndex : testsToRunIndexes )
        {
            if( iTestIndex < testListSize )
            {
                printf( "\n\nRunning test %s.\n\n", testList[iTestIndex]->GetTestName().c_str() );
                int returnVal = testList[iTestIndex]->RunTest( isVerbose, disallowTestToPause );
                testList[iTestIndex]->DestroyPeers();

                if( returnVal == 0 )
                {
                    passedTests++;
                }
                else
                {
                    printf( "Test %s returned with error %s", testList[iTestIndex]->GetTestName().c_str(), testList[iTestIndex]->ErrorCodeToString( returnVal ).c_str() );
                }
            }
        }
    }

    if( numTests > 0 )
    {
        printf( "\nPassed %i out of %i tests.\n", passedTests, numTests );
    }

    //Cleanup
    for( TestInterface* pTest : testList )
    {
        delete pTest;
    }
    testList.clear();

    return 0;
}
