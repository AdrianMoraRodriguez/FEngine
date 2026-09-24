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

target_link_libraries(FEngine PUBLIC
                        glfw
                        glm::glm
                        Vulkan::Vulkan
                        vk-bootstrap::vk-bootstrap
                        GPUOpen::VulkanMemoryAllocator)

target_compile_definitions(FEngine PUBLIC
                            GLM_FORCE_DEPTH_ZERO_TO_ONE
                            GLM_FORCE_RADIANS
                            GLM_ENABLE_EXPERIMENTAL)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES})
