cmake_minimum_required(VERSION 2.6)

set( RAKNET_INTERNAL_INCLUDE_DIRS 
	${RakNet_SOURCE_DIR}/Source 
)

if( RAKNET_ENABLE_STATIC )
	add_subdirectory(LibStatic)
endif()


if( RAKNET_ENABLE_DLL )
	add_subdirectory(DLL)
endif()

FILE(GLOB ALL_HEADER_SRCS ${RakNet_SOURCE_DIR}/Source/*.h)
INSTALL(FILES ${ALL_HEADER_SRCS} DESTINATION include/raknet)
