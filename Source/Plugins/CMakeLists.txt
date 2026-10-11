set( CURRENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} )

set( CURRENT_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/MessageFilter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughServer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionCommon.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionServer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketConsoleLogger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketFileLogger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketLogger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketOutputWindowLogger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RPC4Plugin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetTransport2.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RelayPlugin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Router2.h
    ${CMAKE_CURRENT_SOURCE_DIR}/StatisticsHistory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TelnetTransport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ThreadsafePacketLogger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TwoWayAuthentication.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPForwarder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyCommon.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyCoordinator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyServer.h
)

set( CURRENT_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/MessageFilter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatPunchthroughServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionCommon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NatTypeDetectionServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketConsoleLogger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketFileLogger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketLogger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketOutputWindowLogger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RPC4Plugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RakNetTransport2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RelayPlugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Router2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StatisticsHistory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TelnetTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ThreadsafePacketLogger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TwoWayAuthentication.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPForwarder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyCoordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UDPProxyServer.cpp
)

set( TARGET_HEADER ${TARGET_HEADER} ${CURRENT_HEADER} PARENT_SCOPE )
set( TARGET_SOURCE ${TARGET_SOURCE} ${CURRENT_SOURCE} PARENT_SCOPE )

install( FILES ${CURRENT_HEADER} DESTINATION include/RakNet/Plugins )
