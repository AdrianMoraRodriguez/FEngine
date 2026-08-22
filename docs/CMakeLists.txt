find_package(Doxygen)

if(NOT DOXYGEN_FOUND)
    message(STATUS "Doxygen not found: the 'docs' target will be unavailable")
    return()
endif()

# Paths are quoted individually so that spaces in the source directory
# do not split them into separate tokens.
set(DOXYGEN_INPUT_DIR   "\"${CMAKE_SOURCE_DIR}/engine\" \"${CMAKE_CURRENT_SOURCE_DIR}/pages\"")
set(DOXYGEN_OUTPUT_DIR  "\"${CMAKE_BINARY_DIR}/docs\"")
set(DOXYGEN_AWESOME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/doxygen-awesome-css")
set(DOXYGEN_HEADER      "\"${CMAKE_CURRENT_SOURCE_DIR}/header.html\"")
set(DOXYGEN_MAINPAGE    "\"${CMAKE_CURRENT_SOURCE_DIR}/pages/mainpage.md\"")

set(DOXYFILE_IN  "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in")
set(DOXYFILE_OUT "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")

configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)

add_custom_target(docs
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating documentation with Doxygen"
    VERBATIM)
