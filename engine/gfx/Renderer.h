#pragma once

/**
 * @file Renderer.h
 * @brief Swapchain management and per-frame rendering resources.
 */

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace fe {

class VulkanContext;
class Window;

/**
 * @brief Per-frame GPU resources.
 *
 * Each frame in flight owns its own command pool and synchronisation
 * primitives. This allows the CPU to record the next frame while the
 * GPU is still executing the previous one.
 */
struct FrameData {
    VkCommandPool   commandPool   = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
};

/**
 * @brief Manages the swapchain, per-frame resources, and the frame loop.
 *
 * Exposes a beginFrame() / endFrame() pair. Between those two calls the
 * caller records rendering commands into currentCommandBuffer().
 *
 * Uses Vulkan 1.3 dynamic rendering throughout: no VkRenderPass or
 * VkFramebuffer objects are created.
 */
class Renderer {
public:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    /**
     * @brief Creates the swapchain and all per-frame resources.
     *
     * @param ctx    Initialised Vulkan context.
     * @param window Window whose framebuffer the swapchain presents to.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, Window& window);

    /// Destroys all resources. Safe to call if init() was never called.
    void shutdown();

    /**
     * @brief Begins a new frame.
     *
     * @return `false` if the frame must be skipped (minimised, out of date).
     */
    bool beginFrame();

    /**
     * @brief Ends the current frame: submits and presents.
     *
     * Must only be called after a successful beginFrame().
     */
    void endFrame();

    /// The command buffer open for recording during the current frame.
    VkCommandBuffer currentCommandBuffer() const;

    /// @return Current swapchain image extent in pixels.
    VkExtent2D swapchainExtent() const { return m_swapchainExtent; }

    /// @return Current swapchain image format.
    VkFormat swapchainFormat() const { return m_swapchainFormat; }

private:
    void createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    void recreateSwapchain();

    const VulkanContext* m_ctx    = nullptr;
    Window*              m_window = nullptr;

    VkSwapchainKHR           m_swapchain       = VK_NULL_HANDLE;
    VkFormat                 m_swapchainFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_swapchainExtent  = {0, 0};
    std::vector<VkImage>     m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;

    FrameData m_frames[MAX_FRAMES_IN_FLIGHT];
    uint32_t  m_currentFrame = 0;
    uint32_t  m_imageIndex   = 0;
};

} // namespace fe
