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
    ${CMAKE_CURRENT_SOURCE_DIR}/ComprehensiveConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectWithSocketTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CrossConnectionConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DroppedConnectionConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EightPeerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GuidGenerationTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LocalIsConnectedTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManyClientsOneServerBlockingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManyClientsOneServerDeallocateBlockingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManyClientsOneServerNonBlockingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MaximumConnectTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MiscellaneousTestsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OfflineMessagesConvertTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketAndLowLevelTestsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerConnectDisconnectTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerConnectDisconnectWithCancelPendingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PingTestsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliableOrderedConvertedTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SecurityFunctionsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SystemAddressAndGuidTest.cpp
)

# Support code shared by the tests.
set( RAKNET_TEST_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/CommonFunctions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommonFunctions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionWaits.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ConnectionWaits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerScope.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PeerScope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetStringMakers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakTimer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakTimer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestHelpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TestHelpers.cpp
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
# Roughly 3 1/2 minutes instead of 4 1/2, and worth much less than it used to be.
# A convenience, never the default: [slow] is the likeliest place a regression
# hides. ADD_TAGS_AS_LABELS below is what makes -LE work with no extra wiring.
#
# One test carries the tag now, where three did before GUID generation stopped
# sleeping: ManyClientsOneServerDeallocateBlocking, measured in Release at 54.8, 56.2
# and 60.2 s across three runs of a 273.6-276.7 s suite. The rule is "over 60 s in
# Release" and that test straddles it - a 5.4 s spread with the top of it over the
# line, and 64.4 s in Debug - so it keeps the tag deliberately; the file header says
# why at length. ManyClientsOneServerNonBlocking (36 s) and
# ManyClientsOneServerBlocking (28 s) are clear of the threshold and lost the tag.
#
# For the record, those three cost 169.29 s, 99.47 s and 91.75 s of a 533.94 s
# suite before the fix - 68% of it.
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
        RESOURCE_LOCK  raknet_udp_ports

        # Uniform by necessity: discovered test names do not exist at configure
        # time, so a single test cannot be given its own timeout without a second
        # TEST_SPEC'd discovery call. One number has to clear every test in both
        # configurations.
        #
        # The bound to clear is the slowest single measurement, not a product of
        # slowest-in-Release and worst-inflation - the slowest test is not the one
        # that inflates. Slowest anywhere is the deallocate test in Release,
        # 169.29 s, and it is *faster* in Debug at 148.51 s; the worst Debug
        # inflation is SecurityFunctions, 41.75 s against 22.55 s. So 300 leaves a
        # factor of 1.77 on the worst real case.
        #
        # Deliberately on the generous side. A too-tight timeout turns a slow but
        # healthy test red, which is the exact false signal this suite exists to
        # avoid; a too-loose one only wastes seconds once. Every wait in Tests/ is
        # bounded and fails with a message of its own at its deadline, so a test
        # reaching 300 s is a hang nothing inside the suite can explain.
        TIMEOUT        300
)
