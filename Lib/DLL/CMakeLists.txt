cmake_minimum_required(VERSION 2.6)
project(RakNetDLL)

FILE(GLOB ALL_HEADER_SRCS ${RakNet_SOURCE_DIR}/Source/*.h)
FILE(GLOB ALL_CPP_SRCS ${RakNet_SOURCE_DIR}/Source/*.cpp)

include_directories( ${RAKNET_INTERNAL_INCLUDE_DIRS} ) 

add_library(RakNetDLL SHARED ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} readme.txt)

target_link_libraries (RakNetDLL ${RAKNET_LIBRARY_LIBS})

install(TARGETS RakNetDLL
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
    )
