cmake_minimum_required( VERSION 3.8 )

project( RakNet )

IF (WIN32 AND NOT UNIX)
	set(RAKNET_LIBRARY_LIBS ws2_32.lib)
ELSE(WIN32 AND NOT UNIX)
	set(RAKNET_LIBRARY_LIBS pthread)
ENDIF(WIN32 AND NOT UNIX)

set(CMAKE_DEBUG_POSTFIX "_d")

# Options
option( RAKNET_ENABLE_DLL       "Generate the DLL project if true."                     TRUE )
option( RAKNET_ENABLE_STATIC    "Generate the static library project if true."          TRUE )
option( RAKNET_ENABLE_PLUGINS   "Include sources from Plugins directory in the build."  TRUE )
option( RAKNET_ENABLE_TESTS     "Generate the test project if true. Needs static lib!"  TRUE )

add_subdirectory( Source )

if( RAKNET_ENABLE_TESTS AND RAKNET_ENABLE_STATIC )
    add_subdirectory( Tests )
endif()
