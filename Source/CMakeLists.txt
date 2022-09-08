cmake_minimum_required(VERSION 2.6)

file( GLOB ALL_HEADER_SRCS  ${RakNet_SOURCE_DIR}/Source/*.h )
file( GLOB ALL_CPP_SRCS     ${RakNet_SOURCE_DIR}/Source/*.cpp   )

include_directories( ${RakNet_SOURCE_DIR}/Source ) 

# create dynamic library if enabled
if( RAKNET_ENABLE_DLL )

    add_library( RakNetDLL SHARED ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} )

    target_compile_definitions( RakNetDLL PUBLIC _RAKNET_DLL )
    target_link_libraries( RakNetDLL ${RAKNET_LIBRARY_LIBS} )

    install( TARGETS RakNetDLL
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
    )

endif()

# create static library if enabled
if( RAKNET_ENABLE_STATIC )

    add_library( RakNetLibStatic STATIC ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} )

    target_compile_definitions( RakNetLibStatic PUBLIC _RAKNET_LIB )
    target_link_libraries( RakNetLibStatic ${RAKNET_LIBRARY_LIBS} )

    install( TARGETS RakNetLibStatic
        ARCHIVE DESTINATION lib
    )

endif()

install( FILES ${ALL_HEADER_SRCS} DESTINATION include/raknet )
