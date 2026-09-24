/**
 * @file main.cpp
 * @brief Entry point of the FEngine editor.
 */

#include "core/Log.h"
#include "core/Version.h"
#include "platform/Window.h"
#include "gfx/VulkanContext.h"

int main() {
    FE_INFO("%s", fe::engineVersionString());

    // Window must be created before the Vulkan context: the context needs
    // the native window handle to create the presentation surface.
    fe::Window window;
    if (!window.init(fe::WindowSpec{})) {
        return 1;
    }

    // Vulkan context must be created before any other Vulkan objects: the
    // context owns the VkInstance and VkDevice, which are required to create
    // any other Vulkan objects.
    fe::VulkanContextSpec vkSpec;

    fe::VulkanContext vkCtx;
    if (!vkCtx.init(window, vkSpec)) {
        return 1;
    }

    while (!window.shouldClose()) {
        window.pollEvents();

        if (window.consumeResizeFlag()) {
            FE_TRACE("Framebuffer resized to %d x %d",
                     window.width(), window.height());
        }
    }

    FE_INFO("Shutting down");

    // Explicit shutdown order: Vulkan context before window.
    // The context destructor calls shutdown() automatically, but being
    // explicit here makes the intended order clear to the reader.
    vkCtx.shutdown();
    window.shutdown();

    return 0;
}
