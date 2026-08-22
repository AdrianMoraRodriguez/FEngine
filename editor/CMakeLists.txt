file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_executable(FEditor ${EDITOR_SOURCES})

# PRIVATE: nadie enlaza con el ejecutable, así que nada se propaga
target_link_libraries(FEditor PRIVATE FEngine)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_SOURCES})