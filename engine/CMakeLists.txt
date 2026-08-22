file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(FEngine STATIC ${ENGINE_SOURCES})

target_include_directories(FEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(glfw3 CONFIG REQUIRED)

target_link_libraries(FEngine PUBLIC glfw)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES})
