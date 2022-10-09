if( RAKNET_ENABLE_PLUGINS )
add_subdirectory( Plugins )
endif()

file( GLOB CURRENT_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/*.h )
file( GLOB CURRENT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp )

set( TARGET_HEADER ${TARGET_HEADER} ${CURRENT_HEADER} )
set( TARGET_SOURCE ${TARGET_SOURCE} ${CURRENT_SOURCE} )

include_directories( ${RakNet_SOURCE_DIR}/Source )

source_group( TREE ${RakNet_SOURCE_DIR}/Source PREFIX "Source" FILES ${TARGET_HEADER} ${TARGET_SOURCE} )

# create dynamic library if enabled
if( RAKNET_ENABLE_DLL )

    add_library( RakNetDLL SHARED ${TARGET_HEADER} ${TARGET_SOURCE} )

    target_compile_definitions( RakNetDLL PUBLIC _RAKNET_DLL )
    target_link_libraries( RakNetDLL ${RAKNET_LIBRARY_LIBS} )

    install( TARGETS RakNetDLL
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
    )

endif()

# create static library if enabled
if( RAKNET_ENABLE_STATIC )

    add_library( RakNetLibStatic STATIC ${TARGET_HEADER} ${TARGET_SOURCE} )

    target_compile_definitions( RakNetLibStatic PUBLIC _RAKNET_LIB )
    target_link_libraries( RakNetLibStatic ${RAKNET_LIBRARY_LIBS} )

    install( TARGETS RakNetLibStatic
        ARCHIVE DESTINATION lib
    )

endif()

install( FILES ${TARGET_HEADER} DESTINATION include/raknet )
