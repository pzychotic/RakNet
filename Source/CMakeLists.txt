if( RAKNET_ENABLE_PLUGINS )
    add_subdirectory( Plugins )
endif()

set( CURRENT_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/BitStream.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CCRakNetSlidingWindow.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandParserInterface.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ConsoleServer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/DR_SHA1.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Export.h
    ${CMAKE_CURRENT_SOURCE_DIR}/GetTime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanEncodingTree.h
    ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanEncodingTreeNode.h
    ${CMAKE_CURRENT_SOURCE_DIR}/InternalPacket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LinuxStrings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LogCommandParser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/MTUSize.h
    ${CMAKE_CURRENT_SOURCE_DIR}/MessageIdentifiers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NativeFeatureIncludes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NativeFeatureIncludesOverrides.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketPriority.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketizedTCP.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PlatformRandom.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginInterface2.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakAssert.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakMemoryOverride.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetCommandParser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetDefines.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetDefinesOverrides.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetSocket2.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetStatistics.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetTime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetTypes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetVersion.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakPeer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakPeerInterface.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakThread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliabilityLayer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SequenceRanges.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SignaledEvent.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketDefines.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketIncludes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketLayer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StringCompressor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StringUtils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SuperFastHash.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPByteBuffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterface.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ThreadsafeQueue.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TransportInterface.h
    ${CMAKE_CURRENT_SOURCE_DIR}/WSAStartupSingleton.h
    ${CMAKE_CURRENT_SOURCE_DIR}/WindowsIncludes.h
)

set( CURRENT_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/BitStream.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CCRakNetSlidingWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandParserInterface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConsoleServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DR_SHA1.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GetTime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanEncodingTree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LinuxStrings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LogCommandParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketizedTCP.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PlatformRandom.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginInterface2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakMemoryOverride.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetCommandParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetSocket2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetSocket2_Berkley.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetStatistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakPeer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ReliabilityLayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SignaledEvent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SocketLayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StringCompressor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StringUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SuperFastHash.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPByteBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TCPInterface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/WSAStartupSingleton.cpp
)

# Internal headers build with the library but are not installed.
set( INTERNAL_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/AddrInfo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ParseWholeNumber.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TotalListLength.h
)

set( TARGET_HEADER ${TARGET_HEADER} ${CURRENT_HEADER} ${INTERNAL_HEADER} )
set( TARGET_SOURCE ${TARGET_SOURCE} ${CURRENT_SOURCE} )

include_directories( ${RakNet_SOURCE_DIR}/Source )

source_group( TREE ${RakNet_SOURCE_DIR}/Source PREFIX "Source" FILES ${TARGET_HEADER} ${TARGET_SOURCE} )

# Applies to the libraries built from Source/, not to Tests/.
set( RAKNET_SOURCE_COMPILE_OPTIONS "" CACHE STRING "Extra compile options for the RakNet libraries only." )
add_compile_options( ${RAKNET_SOURCE_COMPILE_OPTIONS} )

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
