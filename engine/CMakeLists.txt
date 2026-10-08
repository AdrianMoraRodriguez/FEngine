file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(FEngine STATIC ${ENGINE_SOURCES})

target_include_directories(FEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_link_libraries(FEngine PUBLIC
                        glfw
                        glm::glm
                        Vulkan::Vulkan
                        vk-bootstrap::vk-bootstrap
                        GPUOpen::VulkanMemoryAllocator
                        EnTT::EnTT
                        nlohmann_json::nlohmann_json)

target_link_libraries(FEngine PRIVATE imgui::imgui)

target_compile_definitions(FEngine PUBLIC
                            GLM_FORCE_DEPTH_ZERO_TO_ONE
                            GLM_FORCE_RADIANS
                            GLM_ENABLE_EXPERIMENTAL)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES})

# ---------------------------------------------------------------------------
# Shader compilation
# Finds glslc and compiles every .vert/.frag/.comp in shaders/ to SPIR-V.
# The compiled .spv files land next to the executable so relative paths work.
# ---------------------------------------------------------------------------

find_program(GLSLC_EXECUTABLE glslc
    HINTS "$ENV{VULKAN_SDK}/Bin")

if(NOT GLSLC_EXECUTABLE)
    message(WARNING "glslc not found — shaders will not be compiled")
else()
    file(GLOB_RECURSE GLSL_SOURCES
        "${CMAKE_SOURCE_DIR}/shaders/*.vert"
        "${CMAKE_SOURCE_DIR}/shaders/*.frag"
        "${CMAKE_SOURCE_DIR}/shaders/*.comp")

    foreach(GLSL_FILE ${GLSL_SOURCES})
        get_filename_component(FILE_NAME ${GLSL_FILE} NAME)
        set(SPV_FILE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${FILE_NAME}.spv")

        add_custom_command(
            OUTPUT  ${SPV_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders"
            COMMAND ${GLSLC_EXECUTABLE} ${GLSL_FILE} -o ${SPV_FILE}
            DEPENDS ${GLSL_FILE}
            COMMENT "Compiling shader: ${FILE_NAME}"
            VERBATIM)

        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()

    add_custom_target(Shaders ALL DEPENDS ${SPV_FILES})
    add_dependencies(FEngine Shaders)
endif()
