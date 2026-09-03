cmake_minimum_required( VERSION 3.24 )   # 3.24: FetchContent FIND_PACKAGE_ARGS, used by Tests/

project( RakNet )

IF (WIN32 AND NOT UNIX)
	# bcrypt: BCryptGenRandom, the entropy source behind RakNet::FillRandomBytes.
	set(RAKNET_LIBRARY_LIBS ws2_32.lib bcrypt.lib)
ELSE(WIN32 AND NOT UNIX)
	set(RAKNET_LIBRARY_LIBS pthread)
ENDIF(WIN32 AND NOT UNIX)

# Options
option( RAKNET_ENABLE_DLL       "Generate the DLL project if true."                     TRUE )
option( RAKNET_ENABLE_STATIC    "Generate the static library project if true."          TRUE )
option( RAKNET_ENABLE_PLUGINS   "Include sources from Plugins directory in the build."  TRUE )
option( RAKNET_ENABLE_TESTS     "Generate the test project if true. Needs static lib!"  TRUE )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

if( MSVC )
    add_definitions( -D_CRT_SECURE_NO_WARNINGS )
    add_definitions( -D_WINSOCK_DEPRECATED_NO_WARNINGS )
endif()

add_subdirectory( Source )

if( RAKNET_ENABLE_TESTS AND RAKNET_ENABLE_STATIC )
    # Must be called from the top-level directory or ctest cannot run from the build
    # root. Deliberately not include( CTest ), which would also add a BUILD_TESTING
    # cache option and a set of CDash Nightly* targets we do not want.
    enable_testing()

    add_subdirectory( Tests )
endif()
