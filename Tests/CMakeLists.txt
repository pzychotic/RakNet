# ---------------------------------------------------------------------------
# Catch2 v3.16.0
#
# Kept local to Tests/ rather than the root file: RAKNET_ENABLE_TESTS=OFF then
# skips the clone with no second guard, and the whole test dependency - fetch,
# link, discovery - reads top-to-bottom in one place. Only enable_testing() has to
# live at the top level.
#
# FIND_PACKAGE_ARGS must come last: everything after it is passed to find_package().
# 3.16 rather than 3 because Catch2's package version file is SameMajorVersion, so
# a bare 3 would accept 3.0.1, which predates ADD_TAGS_AS_LABELS (added in 3.8.0).
# ---------------------------------------------------------------------------
include( FetchContent )

# Defensive only: as of v3.16.0 every install() in Catch2 is guarded by
# NOT_SUBPROJECT, so a FetchContent'd Catch2 contributes no install rules. Plain
# (non-cache) variables are enough - Catch2's CMP0077 is NEW, so option() honours them.
set( CATCH_INSTALL_DOCS      OFF )
set( CATCH_INSTALL_EXTRAS    OFF )
set( CATCH_DEVELOPMENT_BUILD OFF )

FetchContent_Declare( Catch2
    GIT_REPOSITORY    https://github.com/catchorg/Catch2.git
    GIT_TAG           v3.16.0
    GIT_SHALLOW       TRUE
    # EXCLUDE_FROM_ALL        # would need cmake_minimum_required >= 3.28
    # SYSTEM                  # would need cmake_minimum_required >= 3.25
    FIND_PACKAGE_ARGS 3.16
)
FetchContent_MakeAvailable( Catch2 )

# Only set when Catch2 was actually cloned; empty when FIND_PACKAGE_ARGS found an
# installed Catch2 instead, in which case Catch2Config.cmake has already put its own
# directory on the module path.
if( catch2_SOURCE_DIR )
    list( APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras" )
endif()
include( Catch )

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# Every test file under Tests/ is here.
set( RAKNET_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/AckDelayTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BitStreamRoundTripTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BulkSendResendTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CloseClosingConnectionTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandParserInterfaceTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ComprehensiveConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectAlreadyConnectedTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectWithSocketTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionGettersViewTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionSettersTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionRequestEchoTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CrossConnectionConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DroppedConnectionConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EightPeerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ExternalAddressViewTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GetTimeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GuidGenerationTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/InternalIDTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/InvalidPasswordMessageTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LocalIsConnectedTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManyClientsOneServerBlockingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManyClientsOneServerDeallocateBlockingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManyClientsOneServerNonBlockingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MaximumConnectTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MaximumMessageSizeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MessageFilterTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MiscellaneousTestsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughClientQueuedOpenNatTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughServerDisconnectTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughServerSelfRequestTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionBindTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionRecvLoopTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OfflineMessagesConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketAllocationFailureTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketAndLowLevelTestsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketLoggerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketizedTCPDownloadProgressTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerConnectDisconnectTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerConnectDisconnectWithCancelPendingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PingTestsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginCapsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PublishedViewTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakThreadTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReceiveAfterPublishTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReceiveQueueCapTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliabilityLayerByteBudgetTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliabilityLayerSendParametersTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliableOrderedConvertedTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReplacedCallbackTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Router2EntitlementTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Router2ForwardingFlowTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Router2RequestForwardingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Router2UnrequestedForwardingSuccessTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RPC4LoopbackTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SecurityFunctionsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SequenceRangesWireTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ShutdownNotifiesTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ShutdownStopsReceiveThreadTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SlidingWindowAckTimingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SlidingWindowLossRecoveryTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketQueryTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketSendAddressFamilyTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketTTLOptionTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketWildcardAddressTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SplitPacketReassemblyTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StatisticsHistoryCullOnAddTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StatisticsHistorySortTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StatisticsQueryTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StringCompressorTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StringUtilsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SystemAddressAndGuidTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SystemAddressToStringTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPByteBufferTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPCapsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterfaceBindAddressLengthTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterfaceCloseConnectionByAddressTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterfaceDescriptorZeroTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterfaceFullClientTableTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterfaceStopConnectAttemptTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterfaceStopTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TelnetTransportCloseConnectionTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TelnetTransportLineBufferTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TelnetTransportReconnectTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ThreadsafeQueueTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ThroughputBenchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimestampTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TruncatedConnectionRequestTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TwoWayAuthenticationClosedConnectionTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TwoWayAuthenticationNonceTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyClientEntitlementTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyClientForwardingFlowTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyCoordinatorEntitlementTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyCoordinatorPingOrderTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyCoordinatorRemovalTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyServerEntitlementTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/WSAStartupSingletonTest.cpp
)

# Support code shared by the tests.
set( RAKNET_TEST_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/CommonFunctions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommonFunctions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionWaits.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionWaits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/InspectableTelnetTransport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LoopbackTCP.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LoopbackTCP.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MarkerInjection.h
    ${CMAKE_CURRENT_SOURCE_DIR}/MarkerInjection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerScope.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerScope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetStringMakers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RawSystem.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliabilityLayerHarness.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakTimer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakTimer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StringCompressorScope.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TestHelpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TestHelpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyWire.h
    ${CMAKE_CURRENT_SOURCE_DIR}/WinsockScope.h
)

include_directories( ${RakNet_SOURCE_DIR}/Source )

source_group( TREE ${RakNet_SOURCE_DIR}/Tests PREFIX "Source" FILES ${RAKNET_TEST_SOURCES} )
source_group( TREE ${RakNet_SOURCE_DIR}/Tests PREFIX "Support" FILES ${RAKNET_TEST_SUPPORT_SOURCES} )

# ---------------------------------------------------------------------------
# Target
#
# One binary. Catch2::Catch2WithMain supplies main(), so the suite defines none.
# ---------------------------------------------------------------------------
add_executable( RakNetTests
    ${RAKNET_TEST_SOURCES}
    ${RAKNET_TEST_SUPPORT_SOURCES}
)
target_link_libraries( RakNetTests PRIVATE RakNetStatic Catch2::Catch2WithMain )

# RakNetStringMakers.h is force-included into every TU, not left to each test to
# include. Catch::StringMaker<RakNetGUID> is the same symbol whether it is our
# specialization or the primary template, so one TU that compares a guid without
# the header emits a "{?}" convert, and the linker keeps whichever copy it meets
# first - for every test in the binary. Forcing it makes forgetting impossible.
if( MSVC )
    target_compile_options( RakNetTests PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/RakNetStringMakers.h" )
else()
    target_compile_options( RakNetTests PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/RakNetStringMakers.h" )
endif()

# ---------------------------------------------------------------------------
# ctest registration
#
# Canonical run:
#   ctest --test-dir <build> -C <Config> --output-on-failure --no-tests=error
#
# -C is mandatory: under PRE_TEST the tests file is per-config. --no-tests=error
# turns an empty selection red instead of silently green. Deliberately not passed
# here: TEST_SPEC and --allow-running-no-tests (both make a filter typo pass),
# SKIP_IS_FAILURE, RUN_SERIAL, REPORTER/OUTPUT_DIR.
#
# Dev inner loop, skipping the [slow] tests:
#   ctest --test-dir <build> -C <Config> --output-on-failure --no-tests=error -LE slow
#
# A convenience, never the default: [slow] is the likeliest place a regression
# hides. ADD_TAGS_AS_LABELS below is what makes -LE work with no extra wiring.
#
# The rule is "over 60 s in Release", and no test carries the tag. The longest is
# ManyClientsOneServerDeallocateBlocking at 33.2-34.1 s in Release and Debug, then
# ManyClientsOneServerNonBlocking at 17.1-17.2 s and DroppedConnectionConvert at
# 17.0-17.1 s.
# ---------------------------------------------------------------------------
catch_discover_tests( RakNetTests
    # POST_BUILD, the default, is broken on multi-config generators: it writes one
    # config-agnostic tests file and the last config built wins for every -C.
    DISCOVERY_MODE     PRE_TEST

    # Catch2 tags become ctest labels, so `ctest -L network` works with no wiring.
    ADD_TAGS_AS_LABELS

    WORKING_DIRECTORY  "${CMAKE_CURRENT_BINARY_DIR}"

    # The only place uniform properties can go: discovered test names do not exist
    # at configure time.
    PROPERTIES
        # Ports are hardcoded across ~70 call sites and are one global resource,
        # so no two tests may run at once. Makes -j safe, not faster.
        #
        # Fixed ports sit below 32768, under both Windows' dynamic range
        # (49152-65535) and Linux's ephemeral range (32768-60999), so no outgoing
        # connection on the machine can be holding one. New tests bind port 0 and
        # read the port back with GetMyBoundAddress.
        RESOURCE_LOCK  raknet_udp_ports

        # Uniform by necessity: discovered test names do not exist at configure
        # time, so a single test cannot be given its own timeout without a second
        # TEST_SPEC'd discovery call. One number has to clear every test in every
        # environment the suite runs in: Release and Debug locally, and on CI GCC
        # Release, MSVC Debug and TSAN.
        #
        # The bound to clear is the slowest single measurement. That is
        # ManyClientsOneServerDeallocateBlocking everywhere, at 33.1-34.9 s across
        # all five, and TSAN and Debug barely slow it because its time is fixed
        # windows. So 120 leaves a factor of about 3.4 on the worst real case, and
        # room for a test up to the 60 s [slow] line plus inflation.
        #
        # Deliberately on the generous side. A too-tight timeout turns a slow but
        # healthy test red, which is the exact false signal this suite exists to
        # avoid; a too-loose one only wastes seconds once. Every wait in Tests/ is
        # bounded and fails with a message of its own at its deadline, so a test
        # reaching 120 s is a hang nothing inside the suite can explain.
        TIMEOUT        120
)
