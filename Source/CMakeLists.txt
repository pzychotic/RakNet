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

    add_library( RakNet SHARED ${TARGET_HEADER} ${TARGET_SOURCE} )

    target_compile_definitions( RakNet PUBLIC _RAKNET_DLL )
    target_link_libraries( RakNet ${RAKNET_LIBRARY_LIBS} )

    install( TARGETS RakNet
        RUNTIME DESTINATION bin/$<LOWER_CASE:$<CONFIG>>
        ARCHIVE DESTINATION lib/$<LOWER_CASE:$<CONFIG>>
    )

endif()

# create static library if enabled
if( RAKNET_ENABLE_STATIC )

    add_library( RakNetStatic STATIC ${TARGET_HEADER} ${TARGET_SOURCE} )

    target_compile_definitions( RakNetStatic PUBLIC _RAKNET_LIB )
    target_link_libraries( RakNetStatic ${RAKNET_LIBRARY_LIBS} )

    install( TARGETS RakNetStatic
        ARCHIVE DESTINATION lib/$<LOWER_CASE:$<CONFIG>>
    )

endif()

install( FILES ${CURRENT_HEADER} DESTINATION include/RakNet )
