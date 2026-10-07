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
 *
 * @note imageAvailableSemaphore has been removed from this struct.
 *       Acquisition semaphores are now stored per-frame-slot in
 *       Renderer::m_imageAvailableSemaphores, indexed by m_currentFrame,
 *       to avoid reusing a semaphore still in use by the swapchain.
 */
struct FrameData {
    VkCommandPool   commandPool   = VK_NULL_HANDLE; ///< Pool for commandBuffer.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE; ///< Primary command buffer.

    /// Signalled by the GPU when all rendering commands have finished.
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;

    /// CPU-side fence. Waited on before reusing this frame slot.
    VkFence inFlightFence = VK_NULL_HANDLE;
};

/**
 * @brief Manages the swapchain, per-frame resources, and the frame loop.
 *
 * Exposes a beginFrame() / endFrame() pair. Between those two calls the
 * caller records rendering commands into currentCommandBuffer().
 *
 * Uses Vulkan 1.3 dynamic rendering: no VkRenderPass or VkFramebuffer.
 *
 * ### Semaphore strategy
 * One `imageAvailableSemaphore` per frame slot (not per swapchain image).
 * Because MAX_FRAMES_IN_FLIGHT (2) < swapchain image count (3), no two
 * active frame slots can reference the same swapchain image simultaneously,
 * so the semaphore is always free by the time it is reused.
 *
 * Typical render loop:
 * @code
 * while (!window.shouldClose()) {
 *     window.pollEvents();
 *     if (!renderer.beginFrame()) continue;
 *     // record into renderer.currentCommandBuffer()
 *     renderer.endFrame();
 * }
 * @endcode
 */
class Renderer {
public:
    /// Maximum number of frames the GPU may process concurrently.
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
     * Waits for the in-flight fence of the current slot, acquires a
     * swapchain image, and opens the command buffer for recording.
     * Starts a dynamic rendering pass cleared to the engine background colour.
     *
     * @return `false` if the frame must be skipped (minimised window or
     *         out-of-date swapchain). The caller should `continue`.
     */
    bool beginFrame();

    /**
     * @brief Ends the current frame: submits and presents.
     *
     * Closes the dynamic rendering pass, submits the command buffer,
     * and presents the image. Recreates the swapchain when needed.
     *
     * Must only be called after a successful beginFrame().
     */
    void endFrame();

    /**
     * @brief The command buffer open for recording during the current frame.
     *
     * Only valid between a successful beginFrame() and the matching endFrame().
     */
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

    VkSwapchainKHR           m_swapchain      = VK_NULL_HANDLE;
    VkFormat                 m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_swapchainExtent = {0, 0};
    std::vector<VkImage>     m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;

    /// One semaphore per frame slot. Signalled by the swapchain when the
    /// acquired image is ready to write. Indexed by m_currentFrame.
    std::vector<VkSemaphore> m_imageAvailableSemaphores;

    FrameData m_frames[MAX_FRAMES_IN_FLIGHT];
    uint32_t  m_currentFrame = 0;
    uint32_t  m_imageIndex   = 0;
};

} // namespace fe
