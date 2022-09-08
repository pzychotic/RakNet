cmake_minimum_required(VERSION 2.6)
project(RakNetLibStatic)

FILE(GLOB ALL_HEADER_SRCS ${RakNet_SOURCE_DIR}/Source/*.h)
FILE(GLOB ALL_CPP_SRCS ${RakNet_SOURCE_DIR}/Source/*.cpp)

include_directories( ${RAKNET_INTERNAL_INCLUDE_DIRS} ) 

add_library(RakNetLibStatic STATIC ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} readme.txt)

target_compile_definitions( RakNetLibStatic PUBLIC _RAKNET_LIB )
target_link_libraries (RakNetLibStatic ${RAKNET_LIBRARY_LIBS})

install(TARGETS RakNetLibStatic 
        ARCHIVE DESTINATION lib
    )
