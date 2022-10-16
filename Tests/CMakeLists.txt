file( GLOB TEST_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.h )
file( GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp )

include_directories( ${RakNet_SOURCE_DIR}/Source ) 

add_executable( RakNetTests ${TEST_HEADERS} ${TEST_SOURCES} )

target_link_libraries( RakNetTests RakNetStatic )
