file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(FEngine STATIC ${ENGINE_SOURCES})

# PUBLIC: quien enlace con FEngine hereda este include path.
# Por eso el editor podrá escribir #include "core/Version.h"
target_include_directories(FEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Reproduce el árbol de carpetas dentro del Solution Explorer de VS
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES})