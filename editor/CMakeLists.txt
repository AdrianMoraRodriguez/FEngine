file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_executable(FEditor ${EDITOR_SOURCES})

find_package(imgui CONFIG REQUIRED)

# PRIVATE: nadie enlaza con el ejecutable, así que nada se propaga
target_link_libraries(FEditor PRIVATE FEngine imgui::imgui)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_SOURCES})

if(WIN32)
    target_sources(FEditor PRIVATE resources/fractalis.rc)
endif()

# Window icon PNGs, next to the executable.
add_custom_command(TARGET FEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/branding/png"
            "$<TARGET_FILE_DIR:FEditor>/branding")
