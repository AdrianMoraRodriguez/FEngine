/**
 * @file VulkanContext.h
 * @brief Core Vulkan objects shared across the entire renderer.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace fe {
  class Window;

  /**
   * @brief Configuration for VulkanContext initialisation.
   *
   */
  struct VulkanContextSpec {
    /// Application name embedded in the VkInstance.
    const char* appName = "Fractalis Engine Editor";

    /// Enable the Vulkan validation layers and debug messenger. Should be true in debug builds, false in release builds.
    bool enableValidation = true;
  };

  class VulkanContext {
   public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /**
     * @brief Creates all core Vulkan objects.
     *
     * @param window The window whose native handle is used to create the
     *               presentation surface. Must remain valid for the lifetime
     *               of this context.
     * @param spec  Creation parameters. Defaults are appropriate for a debug
     *              build.
     * @return `true` on success. On failure the reason is logged and the
     *         object is safe to destroy.
     */
    bool init(const Window& window, const VulkanContextSpec& spec);

    /**
     * @brief Destroys all Vulkan objects in reverse creation order.
     *
     * Safe to call when init() was never called or failed. Waits for the device to finish all work before destroying it
     */
    void shutdown();

    /// @return The Vulkan instance.
    VkInstance instance() const { return m_instance; }

    /// @return The selected physical device (GPU).
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }

    /// @return The logical device used to create all other Vulkan objects.
    VkDevice device() const { return m_device; }

    /// @return The window presentation surface.
    VkSurfaceKHR surface() const { return m_surface; }

    /**
     * @brief The queue used to submit graphics and compute work.
     *
     * On most GPU this queue also supports presentation, but
     * presentQueue() should always be used for present submissions
     */
    VkQueue graphicsQueue() const { return m_graphicsQueue; }

    /// @return The queue used to present frames to the swapchain.
    VkQueue presentQueue() const { return m_presentQueue; }

    /// @return Index of the graphics queue family.
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }

    /// @return Index of the present queue family.
    uint32_t presentQueueFamily() const { return m_presentQueueFamily; }
   private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    uint32_t m_graphicsQueueFamily = 0;
    uint32_t m_presentQueueFamily = 0;
  };
} // namespace fe
